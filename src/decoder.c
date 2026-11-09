/* ffmpeg decoder child processes; see decoder.h. */
#include "decoder.h"
#include "sink.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#ifndef LEGACY_MAC
#include <spawn.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

#define IN_QUEUE (8u << 20) /* ~5 s of the highest bitrate */

struct decoder {
#ifdef _WIN32
    HANDLE proc, in, out;
#else
    pid_t pid;
    int in, out;
#endif
    sink_t *feed;
    size_t chunk;
    dec_out_cb cb;
    void *user;
    pthread_t reader;
};

/* ---- platform ---- */

#ifdef _WIN32

static int is_file(const char *utf8)
{
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, MAX_PATH * 2))
        return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int exe_dir(char *out, size_t len)
{
    wchar_t w[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2)
        return -1;
    wchar_t *slash = wcsrchr(w, L'\\');
    if (slash)
        *slash = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)len, NULL, NULL) ? 0 : -1;
}

#define EXE_NAME "ffmpeg.exe"
#define PATH_SEP ";"
#define DIR_SEP "\\"
#define EXTRA_DIRS ""

/* Command line quoting that CommandLineToArgvW and the MSVC runtime undo. */
static void append_arg(wchar_t *cmd, size_t cap, const char *arg)
{
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, 2048);
    size_t n = wcslen(cmd);
#define PUT(c) do { if (n + 1 < cap) cmd[n++] = (c); } while (0)
    if (n)
        PUT(L' ');
    PUT(L'"');
    for (const wchar_t *p = w;; p++) {
        size_t bs = 0;
        while (*p == L'\\') {
            p++;
            bs++;
        }
        if (!*p) {
            for (size_t i = 0; i < bs * 2; i++) PUT(L'\\');
            break;
        }
        if (*p == L'"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) PUT(L'\\');
        } else {
            for (size_t i = 0; i < bs; i++) PUT(L'\\');
        }
        PUT(*p);
    }
    PUT(L'"');
#undef PUT
    cmd[n] = 0;
}

static pthread_mutex_t g_spawn_lock = PTHREAD_MUTEX_INITIALIZER;

static int spawn(decoder_t *d, const char *ffmpeg, const char *const *args, char *err, int errlen)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE in_r = NULL, out_w = NULL, nul;
    /* one spawn at a time, so a child never inherits another child's pipe ends */
    pthread_mutex_lock(&g_spawn_lock);
    if (!CreatePipe(&in_r, &d->in, &sa, 1 << 20) || !CreatePipe(&d->out, &out_w, &sa, 1 << 20)) {
        pthread_mutex_unlock(&g_spawn_lock);
        snprintf(err, (size_t)errlen, "cannot create a pipe (error %lu)", GetLastError());
        return -1;
    }
    SetHandleInformation(d->in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(d->out, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    static wchar_t cmd[32768];
    cmd[0] = 0;
    append_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), ffmpeg);
    for (int i = 0; args[i]; i++)
        append_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), args[i]);
    wchar_t wexe[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, ffmpeg, -1, wexe, MAX_PATH * 2);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = nul;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(wexe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    pthread_mutex_unlock(&g_spawn_lock);
    if (!ok) {
        snprintf(err, (size_t)errlen, "cannot start %s (error %lu)", ffmpeg, e);
        CloseHandle(d->in);
        CloseHandle(d->out);
        return -1;
    }
    CloseHandle(pi.hThread);
    d->proc = pi.hProcess;
    return 0;
}

static int write_in(void *ctx, const uint8_t *p, size_t n)
{
    decoder_t *d = ctx;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(d->in, p, n > (1u << 20) ? (1u << 20) : (DWORD)n, &w, NULL))
            return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static long read_out(decoder_t *d, uint8_t *p, size_t n)
{
    DWORD got = 0;
    if (!ReadFile(d->out, p, (DWORD)n, &got, NULL))
        return -1;
    return (long)got;
}

static void close_in(decoder_t *d)
{
    if (d->in) {
        CloseHandle(d->in);
        d->in = NULL;
    }
}

static void kill_child(decoder_t *d) { TerminateProcess(d->proc, 1); }

static int end_child(decoder_t *d)
{
    DWORD code = 1;
    WaitForSingleObject(d->proc, INFINITE);
    GetExitCodeProcess(d->proc, &code);
    CloseHandle(d->proc);
    return code == 0 ? 0 : -1;
}

static void close_out(decoder_t *d) { CloseHandle(d->out); }

#else /* POSIX */

static int is_file(const char *path) { return access(path, X_OK) == 0; }

static int exe_dir(char *out, size_t len)
{
    char buf[4096];
#ifdef __APPLE__
    uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n))
        return -1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1;
    buf[n] = 0;
#else
    return -1;
#endif
    char *slash = strrchr(buf, '/');
    if (slash)
        *slash = 0;
    snprintf(out, len, "%s", buf);
    return 0;
}

#define EXE_NAME "ffmpeg"
#define PATH_SEP ":"
#define DIR_SEP "/"
/* GUI apps get a minimal PATH, so also try the usual Homebrew/MacPorts places */
#define EXTRA_DIRS ":/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:/usr/bin"

static int spawn(decoder_t *d, const char *ffmpeg, const char *const *args, char *err, int errlen)
{
    int ip[2], op[2];
    if (pipe(ip)) {
        snprintf(err, (size_t)errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    if (pipe(op)) {
        snprintf(err, (size_t)errlen, "pipe: %s", strerror(errno));
        close(ip[0]);
        close(ip[1]);
        return -1;
    }
    fcntl(ip[1], F_SETFD, FD_CLOEXEC);
    fcntl(op[0], F_SETFD, FD_CLOEXEC);

    const char *argv[64];
    int n = 0;
    argv[n++] = ffmpeg;
    for (int i = 0; args[i] && n < 63; i++)
        argv[n++] = args[i];
    argv[n] = NULL;

#ifdef LEGACY_MAC
    /* Mac OS X 10.3/10.4 have no posix_spawn (10.5) */
    int rc = 0;
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        dup2(ip[0], 0);
        dup2(op[1], 1);
        if (nul >= 0)
            dup2(nul, 2);
        execv(ffmpeg, (char *const *)argv);
        _exit(127);
    }
    if (pid < 0)
        rc = errno;
    else
        d->pid = pid;
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, ip[0], 0);
    posix_spawn_file_actions_adddup2(&fa, op[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int rc = posix_spawn(&d->pid, ffmpeg, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
#endif
    close(ip[0]);
    close(op[1]);
    if (rc) {
        snprintf(err, (size_t)errlen, "cannot start %s: %s", ffmpeg, strerror(rc));
        close(ip[1]);
        close(op[0]);
        return -1;
    }
    d->in = ip[1];
    d->out = op[0];
    signal(SIGPIPE, SIG_IGN); /* a dying ffmpeg must not kill the app */
    return 0;
}

static int write_in(void *ctx, const uint8_t *p, size_t n)
{
    decoder_t *d = ctx;
    while (n) {
        ssize_t w = write(d->in, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static long read_out(decoder_t *d, uint8_t *p, size_t n)
{
    for (;;) {
        ssize_t r = read(d->out, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        return (long)r;
    }
}

static void close_in(decoder_t *d)
{
    if (d->in >= 0) {
        close(d->in);
        d->in = -1;
    }
}

static void kill_child(decoder_t *d) { kill(d->pid, SIGKILL); }

static int end_child(decoder_t *d)
{
    int status = 1;
    while (waitpid(d->pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void close_out(decoder_t *d) { close(d->out); }

#endif

const char *dec_find_ffmpeg(void)
{
    static char found[4096];
    const char *env = getenv("FWDV_FFMPEG");
    if (env && is_file(env))
        return env;
    char dir[4096];
    if (exe_dir(dir, sizeof(dir)) == 0) {
        snprintf(found, sizeof(found), "%s" DIR_SEP EXE_NAME, dir);
        if (is_file(found))
            return found;
    }
    const char *path = getenv("PATH");
    char buf[8192];
    snprintf(buf, sizeof(buf), "%s" EXTRA_DIRS, path ? path : "");
    for (char *save = NULL, *d = strtok_r(buf, PATH_SEP, &save); d; d = strtok_r(NULL, PATH_SEP, &save)) {
        snprintf(found, sizeof(found), "%s" DIR_SEP EXE_NAME, d);
        if (is_file(found))
            return found;
    }
    return NULL;
}

static void *reader(void *arg)
{
    decoder_t *d = arg;
    size_t cap = d->chunk ? d->chunk : 64 * 1024, have = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        long r = read_out(d, buf + have, cap - have);
        if (r <= 0)
            break;
        have += (size_t)r;
        if (!d->chunk || have == cap) {
            d->cb(buf, have, d->user);
            have = 0;
        }
    }
    free(buf);
    return NULL;
}

decoder_t *dec_start(const char *const *args, size_t chunk, dec_out_cb cb, void *user, char *err, int errlen)
{
    return dec_start_queue(args, chunk, cb, user, IN_QUEUE, err, errlen);
}

static void discard(const uint8_t *data, size_t len, void *user) { (void)data, (void)len, (void)user; }

decoder_t *dec_start_queue(const char *const *args, size_t chunk, dec_out_cb cb, void *user, size_t queue, char *err,
                           int errlen)
{
    const char *ffmpeg = dec_find_ffmpeg();
    if (!ffmpeg) {
        snprintf(err, (size_t)errlen,
#ifdef _WIN32
                 "ffmpeg.exe not found (it ships next to this program; or put one on PATH, or set FWDV_FFMPEG)"
#elif defined(__APPLE__)
                 "ffmpeg not found (it ships inside FireWireRecorder.app; or brew install ffmpeg, or set FWDV_FFMPEG)"
#else
                 "ffmpeg not found (it ships next to this program; or install the ffmpeg package, or set FWDV_FFMPEG)"
#endif
        );
        return NULL;
    }
    decoder_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->chunk = chunk;
    d->cb = cb ? cb : discard;
    d->user = user;
    if (spawn(d, ffmpeg, args, err, errlen)) {
        free(d);
        return NULL;
    }
    d->feed = sink_start(queue, write_in, d);
    if (!d->feed || pthread_create(&d->reader, NULL, reader, d)) {
        sink_stop(d->feed, 0);
        close_in(d);
        kill_child(d);
        end_child(d);
        close_out(d);
        free(d);
        snprintf(err, (size_t)errlen, "out of memory");
        return NULL;
    }
    return d;
}

int dec_feed(decoder_t *d, const uint8_t *data, size_t len) { return sink_push(d->feed, data, len); }

uint64_t dec_dropped(decoder_t *d) { return d ? sink_dropped(d->feed) : 0; }

int dec_finish(decoder_t *d)
{
    if (!d)
        return -1;
    int ok = sink_stop(d->feed, 1) == 0; /* everything queued goes in first */
    close_in(d);                         /* end of input: ffmpeg finishes the file and exits */
    ok &= end_child(d) == 0;
    pthread_join(d->reader, NULL);
    close_out(d);
    free(d);
    return ok ? 0 : -1;
}

void dec_stop(decoder_t *d)
{
    if (!d)
        return;
    /* a decoder has nothing worth finishing: end it at once, which also
     * unblocks a feed write or a read that is waiting on it */
    kill_child(d);
    sink_stop(d->feed, 0);
    close_in(d);
    end_child(d);
    pthread_join(d->reader, NULL);
    close_out(d);
    free(d);
}
