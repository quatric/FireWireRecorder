/* FireWireRecorder engine; see app_core.h. */
#include "app_core.h"
#include "decoder.h"
#include "monitor_ring.h"
#include "sink.h"
#if !defined(__APPLE__) || defined(LEGACY_MAC) /* the old-Mac build plays through Core Audio here */
#include "sysaudio.h"
#endif

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <sys/stat.h>
#define DIR_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#define DIR_SEP '/'
#endif

/* Monotonic clock. clock_gettime() only exists from macOS 10.12, so macOS
   uses mach_absolute_time(), which is there on every version. */
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
static void mono_ts(struct timespec *ts)
{
#ifdef __APPLE__
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    uint64_t ns = t / tb.denom * tb.numer + t % tb.denom * tb.numer / tb.denom;
    ts->tv_sec = (time_t)(ns / 1000000000u);
    ts->tv_nsec = (long)(ns % 1000000000u);
#elif defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    ts->tv_sec = (time_t)(c.QuadPart / f.QuadPart);
    ts->tv_nsec = (long)(c.QuadPart % f.QuadPart * 1000000000 / f.QuadPart);
#else
    clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

#define RING_FRAMES 32768     /* power of two, ~680 ms at 48 kHz */
/* Decoded preview frames are queued and shown one per frame interval, because
 * ffmpeg hands them over in bursts. */
#define FRAME_QUEUE 8
#define QUEUE_KEEP 5  /* more queued than this: the preview has fallen behind, skip ahead */
#define QUEUE_START 3 /* frames in hand before the preview starts playing */
#define AUDIO_RATE 48000
/* ffmpeg's output in the CPU's own byte order: pixels as 32-bit 0x00RRGGBB, samples as int16 */
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define PIX_FMT "0rgb"
#define PCM_FMT "s16be"
#else
#define PIX_FMT "bgr0"
#define PCM_FMT "s16le"
#endif
#define REC_QUEUE (256u << 20) /* DV is 3.6 MB/s: over a minute for a slow disk or encoder */
#define RETRY_MS 2000
#define NO_PICTURE_MS 1500    /* this long without a frame: the tape has stopped */
#define AUTO_STOP_MS 3000     /* and this long during a recording with "stop at end of tape" */

const char *const app_format_names[APP_FORMATS] = { "Raw DV", "MPEG-2", "x264" };
const char *const app_format_ext[APP_FORMATS] = { "dv", "mpg", "mp4" };

struct app {
    pthread_mutex_t lock; /* state, status, signal, tape status */
    pthread_cond_t cv;
    pthread_t worker;
    volatile int quit, retry;
    app_state_t state;
    char status[256], problem[256], name[128];
    int sig_w, sig_h, sig_fps;
    fwdv_status_t tape;
    int have_tape_status;
    fwdv_t *dev;
    pthread_mutex_t dev_lock; /* AV/C commands from the GUI vs. the worker closing the device */

    pthread_mutex_t data_lock; /* the consumers of the stream */
    decoder_t *vdec, *adec;
    volatile int preview;
    int dec_pal;               /* the format the decoders were started for */
    int64_t last_frame_ms;
    volatile int pal, wide;    /* of the latest frame */
    /* recording */
    int rec_format;
    sink_t *rec;               /* raw DV */
    FILE *rec_file;
    decoder_t *enc;            /* MPEG-2 or H.264: ffmpeg writes the file */
    uint64_t rec_frames;
    int rec_pal;
    char rec_path[1024];
    volatile int auto_stop, auto_stop_due;

    pthread_mutex_t frame_lock;
    uint32_t *rgb;            /* the frame on screen */
    uint32_t *q[FRAME_QUEUE]; /* decoded frames waiting for their turn */
    int q_head, q_count, q_playing;
    int64_t q_due_us, period_us;
    int fw, fh;
    uint64_t seq;

    int16_t ring_frames[RING_FRAMES * 2];
    monitor_ring_t ring;
    volatile int peak[2];
    volatile int monitor;
#if !defined(__APPLE__) || defined(LEGACY_MAC)
    sa_output_t *out;
#endif
};

/* ---- small helpers ---- */

static int64_t now_us(void)
{
    struct timespec ts;
    mono_ts(&ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t now_ms(void) { return now_us() / 1000; }

static void set_state(app_t *a, app_state_t st, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_state(app_t *a, app_state_t st, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&a->lock);
    a->state = st;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&a->lock);
}

static void set_problem(app_t *a, const char *s)
{
    pthread_mutex_lock(&a->lock);
    snprintf(a->problem, sizeof(a->problem), "%s", s ? s : "");
    pthread_mutex_unlock(&a->lock);
}

/* Sleeps up to ms; returns early on quit or app_retry(). */
static void nap(app_t *a, int ms)
{
#ifdef _WIN32
    int64_t until = now_ms() + ms;
    pthread_mutex_lock(&a->lock);
    while (!a->quit && !a->retry && now_ms() < until) {
        pthread_mutex_unlock(&a->lock);
        Sleep(20);
        pthread_mutex_lock(&a->lock);
    }
    a->retry = 0;
    pthread_mutex_unlock(&a->lock);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long ns = (long long)tv.tv_usec * 1000 + (long long)ms * 1000000;
    struct timespec until = { tv.tv_sec + (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
    pthread_mutex_lock(&a->lock);
    while (!a->quit && !a->retry)
        if (pthread_cond_timedwait(&a->cv, &a->lock, &until))
            break;
    a->retry = 0;
    pthread_mutex_unlock(&a->lock);
#endif
}

/* ---- decoders: one ffmpeg for the picture, one for the sound ---- */

static void on_video(const uint8_t *data, size_t len, void *user)
{
    app_t *a = user;
    pthread_mutex_lock(&a->frame_lock);
    if (len == (size_t)a->fw * a->fh * 4) {
        if (a->q_count == FRAME_QUEUE) { /* full: drop the oldest */
            a->q_head = (a->q_head + 1) % FRAME_QUEUE;
            a->q_count--;
        }
        memcpy(a->q[(a->q_head + a->q_count) % FRAME_QUEUE], data, len);
        a->q_count++;
    }
    pthread_mutex_unlock(&a->frame_lock);
}

/* Puts the next queued frame on screen when its time has come. frame_lock held. */
static void advance_frame(app_t *a)
{
    int64_t now = now_us();
    if (!a->q_playing) {
        if (a->q_count < QUEUE_START)
            return;
        a->q_playing = 1;
        a->q_due_us = now;
    }
    if (!a->q_count || now + 8000 < a->q_due_us)
        return;
    while (a->q_count > QUEUE_KEEP) {
        a->q_head = (a->q_head + 1) % FRAME_QUEUE;
        a->q_count--;
    }
    uint32_t *t = a->rgb;
    a->rgb = a->q[a->q_head];
    a->q[a->q_head] = t;
    a->q_head = (a->q_head + 1) % FRAME_QUEUE;
    a->q_count--;
    a->seq++;
    /* the camcorder's clock and ours differ a little: run a few percent slow
     * when the queue is nearly empty and fast when it builds up */
    int64_t base = now - a->q_due_us > a->period_us / 2 ? now : a->q_due_us;
    int64_t step = a->period_us;
    if (a->q_count == 0)
        step += step / 20;
    else if (a->q_count >= QUEUE_START)
        step -= step / 20;
    a->q_due_us = base + step;
}

static void on_audio(const uint8_t *data, size_t len, void *user)
{
    app_t *a = user;
    monitor_ring_write(&a->ring, (const int16_t *)(const void *)data, (int)(len / 4), a->peak);
}

static void clear_frame(app_t *a)
{
    pthread_mutex_lock(&a->frame_lock);
    if (a->rgb)
        memset(a->rgb, 0, (size_t)a->fw * a->fh * 4);
    a->q_head = a->q_count = a->q_playing = 0;
    a->seq++;
    pthread_mutex_unlock(&a->frame_lock);
}

static void stop_video(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    decoder_t *v = a->vdec;
    a->vdec = NULL;
    pthread_mutex_unlock(&a->data_lock);
    dec_stop(v);
    clear_frame(a);
}

static void stop_decoders(app_t *a)
{
    stop_video(a);
    pthread_mutex_lock(&a->data_lock);
    decoder_t *au = a->adec;
    a->adec = NULL;
    pthread_mutex_unlock(&a->data_lock);
    dec_stop(au);
}

static int start_video(app_t *a, int pal)
{
    int w = 720, h = pal ? 576 : 480;
    pthread_mutex_lock(&a->frame_lock);
    if (w != a->fw || h != a->fh) {
        uint32_t *n[FRAME_QUEUE + 1];
        int ok = 1;
        for (int i = 0; i <= FRAME_QUEUE; i++)
            ok &= (n[i] = calloc((size_t)w * h, 4)) != NULL;
        if (ok) {
            free(a->rgb);
            a->rgb = n[FRAME_QUEUE];
            for (int i = 0; i < FRAME_QUEUE; i++) {
                free(a->q[i]);
                a->q[i] = n[i];
            }
            a->fw = w;
            a->fh = h;
        } else {
            for (int i = 0; i <= FRAME_QUEUE; i++)
                free(n[i]);
        }
    }
    a->q_head = a->q_count = a->q_playing = 0;
    a->period_us = pal ? 40000 : 1001000 / 30;
    a->seq++;
    pthread_mutex_unlock(&a->frame_lock);

    char vf[200];
    snprintf(vf, sizeof(vf), "yadif=mode=send_frame:parity=auto:deint=interlaced,scale=%d:%d,format=" PIX_FMT, a->fw,
             a->fh);
    const char *vargs[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-fflags", "nobuffer",
                            "-probesize", "300000", "-analyzeduration", "100000", "-f", "dv", "-i", "pipe:0",
                            "-map", "0:v:0", "-an", "-sn", "-vf", vf, "-f", "rawvideo", "-pix_fmt", PIX_FMT,
                            "-flush_packets", "1", "pipe:1", NULL };
    char err[300] = "";
    decoder_t *v = dec_start(vargs, (size_t)a->fw * a->fh * 4, on_video, a, err, sizeof(err));
    if (!v) {
        char msg[400];
        snprintf(msg, sizeof(msg), "No preview: %s", err);
        set_problem(a, msg);
        return -1;
    }
    pthread_mutex_lock(&a->data_lock);
    a->vdec = v;
    pthread_mutex_unlock(&a->data_lock);
    return 0;
}

static void start_decoders(app_t *a, int pal)
{
    set_problem(a, NULL);
    a->dec_pal = pal;
    if (a->preview && start_video(a, pal))
        return;
    const char *aargs[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-fflags", "nobuffer",
                            "-probesize", "300000", "-analyzeduration", "100000", "-f", "dv", "-i", "pipe:0",
                            "-map", "0:a:0", "-vn", "-sn", "-ac", "2", "-ar", "48000", "-f", PCM_FMT,
                            "-flush_packets", "1", "pipe:1", NULL };
    char err[300] = "";
    decoder_t *au = dec_start(aargs, 4 * 480, on_audio, a, err, sizeof(err));
    if (!au) {
        char msg[400];
        snprintf(msg, sizeof(msg), "No sound: %s", err);
        set_problem(a, msg);
    }
    monitor_ring_restart(&a->ring);
    pthread_mutex_lock(&a->data_lock);
    a->adec = au;
    pthread_mutex_unlock(&a->data_lock);
}

/* ---- the DV stream ---- */

/* 16:9 as libdv and ffmpeg read it: the display mode in the VAUX source control pack */
static int frame_is_wide(const uint8_t *f)
{
    const uint8_t *vsc = f + 80 * 5 + 48 + 5;
    if (vsc[0] != 0x61)
        return 0;
    int apt = f[4] & 0x07, disp = vsc[2] & 0x07;
    return disp == 0x02 || (!apt && disp == 0x07);
}

static void on_frame(const uint8_t *frame, size_t len, void *user)
{
    app_t *a = user;
    int pal = len == DV_FRAME_625;
    a->pal = pal;
    a->wide = frame_is_wide(frame);
    pthread_mutex_lock(&a->data_lock);
    a->last_frame_ms = now_ms();
    if (a->rec) {
        sink_push(a->rec, frame, len);
        a->rec_frames++;
    } else if (a->enc) {
        dec_feed(a->enc, frame, len);
        a->rec_frames++;
    }
    if (a->vdec)
        dec_feed(a->vdec, frame, len);
    if (a->adec)
        dec_feed(a->adec, frame, len);
    pthread_mutex_unlock(&a->data_lock);
}

/* ---- the device worker ---- */

/* the preview was switched on or off: start or end the picture's decoder */
static void sync_preview(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    int has_v = a->vdec != NULL;
    pthread_mutex_unlock(&a->data_lock);
    if (a->preview && !has_v)
        start_video(a, a->dec_pal);
    else if (!a->preview && has_v)
        stop_video(a);
}

static void close_device(app_t *a)
{
    stop_decoders(a);
    pthread_mutex_lock(&a->dev_lock);
    fwdv_t *d = a->dev;
    a->dev = NULL;
    pthread_mutex_unlock(&a->dev_lock);
    fwdv_close(d);
    pthread_mutex_lock(&a->lock);
    a->sig_w = a->sig_h = a->sig_fps = 0;
    a->have_tape_status = 0;
    a->name[0] = 0;
    pthread_mutex_unlock(&a->lock);
}

static const char *mode_text(fwdv_mode_t m)
{
    switch (m) {
    case FWDV_MODE_STOPPED: return "Tape stopped";
    case FWDV_MODE_PAUSED: return "Tape paused";
    case FWDV_MODE_REWINDING: return "Rewinding";
    case FWDV_MODE_FFWD: return "Fast forwarding";
    case FWDV_MODE_PLAYING: return "Playing";
    case FWDV_MODE_REVERSE: return "Playing in reverse";
    case FWDV_MODE_RECORDING: return "Recording on the camcorder";
    case FWDV_MODE_CAMERA: return "Camera";
    default: return "";
    }
}

/* FWDV_REPLAY=file.dv stands in for the device (for testing without one):
 * its frames are fed in a loop at the DV frame rate. */
static void replay(app_t *a, const char *path)
{
    FILE *f = app_fopen(path, "rb");
    if (!f) {
        set_state(a, APP_SEARCHING, "Cannot open %s", path);
        return;
    }
    static uint8_t buf[DV_FRAME_625];
    int64_t t0 = now_us(), n = 0;
    int started = -1;
    pthread_mutex_lock(&a->lock);
    snprintf(a->name, sizeof(a->name), "Replay");
    pthread_mutex_unlock(&a->lock);
    while (!a->quit) {
        if (fread(buf, 1, DV_FRAME_525, f) != DV_FRAME_525) {
            rewind(f);
            continue;
        }
        int pal = buf[3] & 0x80;
        if (pal && fread(buf + DV_FRAME_525, 1, DV_FRAME_625 - DV_FRAME_525, f) != DV_FRAME_625 - DV_FRAME_525) {
            rewind(f);
            continue;
        }
        if (started != pal) {
            stop_decoders(a);
            start_decoders(a, pal);
            started = pal;
            pthread_mutex_lock(&a->lock);
            a->sig_w = 720, a->sig_h = pal ? 576 : 480, a->sig_fps = pal ? 25 : 30;
            pthread_mutex_unlock(&a->lock);
            set_state(a, APP_STREAMING, "Replay of %s", path);
        }
        n++;
        int64_t due = t0 + n * (pal ? 40000 : 1001000 / 30), wait = due - now_us();
        if (wait > 0) {
#ifdef _WIN32
            Sleep((DWORD)(wait / 1000));
#else
            usleep((useconds_t)wait);
#endif
        }
        on_frame(buf, pal ? DV_FRAME_625 : DV_FRAME_525, a);
        sync_preview(a);
    }
    fclose(f);
}

static void *worker(void *arg)
{
    app_t *a = arg;
    const char *rp = getenv("FWDV_REPLAY");
    if (rp && *rp) {
        replay(a, rp);
        stop_decoders(a);
        return NULL;
    }
    set_state(a, APP_SEARCHING, "Looking for a DV camcorder or deck on FireWire...");
    while (!a->quit) {
        fwdv_t *dev = NULL;
        int r = fwdv_open(&dev);
        if (r) {
            if (r == FWDV_ERR_BUSY)
                set_state(a, APP_BUSY, "The DV device is in use by another program (iMovie, QuickTime...)");
#ifdef __linux__
            else if (r == FWDV_ERR_ACCESS)
                set_state(a, APP_SEARCHING, "No permission to use /dev/fw* (see README)");
#endif
            else
                set_state(a, APP_SEARCHING, "No DV device found. Connect the camcorder with FireWire and switch it to VCR/Play.");
            nap(a, RETRY_MS);
            continue;
        }
        pthread_mutex_lock(&a->dev_lock);
        a->dev = dev;
        pthread_mutex_unlock(&a->dev_lock);
        pthread_mutex_lock(&a->lock);
        snprintf(a->name, sizeof(a->name), "%s", fwdv_name(dev));
        pthread_mutex_unlock(&a->lock);

        pthread_mutex_lock(&a->data_lock);
        a->last_frame_ms = 0;
        pthread_mutex_unlock(&a->data_lock);
        start_decoders(a, 0);
        if ((r = fwdv_start(dev, on_frame, a))) {
            set_state(a, r == FWDV_ERR_BUSY ? APP_BUSY : APP_SEARCHING, "Cannot receive from %s: %s", fwdv_name(dev),
                      fwdv_strerror(r));
            close_device(a);
            nap(a, RETRY_MS);
            continue;
        }

        int polls = 0, lost = 0;
        int64_t stopped_since = 0;
        while (!a->quit && !lost) {
            nap(a, 500);
            if (a->quit)
                break;
            fwdv_status_t st;
            pthread_mutex_lock(&a->dev_lock);
            int sr = fwdv_status(dev, &st);
            int alive = ++polls % 4 ? 1 : fwdv_alive(dev);
            pthread_mutex_unlock(&a->dev_lock);
            if (sr == FWDV_ERR_GONE || !alive) {
                lost = 1;
                break;
            }
            pthread_mutex_lock(&a->lock);
            if (!sr)
                a->tape = st;
            a->have_tape_status = !sr;
            pthread_mutex_unlock(&a->lock);

            pthread_mutex_lock(&a->data_lock);
            int64_t last = a->last_frame_ms;
            int recording = a->rec || a->enc;
            pthread_mutex_unlock(&a->data_lock);
            int64_t now = now_ms();
            int pal = a->pal, streaming = last && now - last < NO_PICTURE_MS;

            /* the format changed (a PAL tape in a deck that played NTSC, say): restart the decoders */
            if (streaming && pal != a->dec_pal) {
                stop_decoders(a);
                start_decoders(a, pal);
            }
            sync_preview(a);

            pthread_mutex_lock(&a->lock);
            if (streaming)
                a->sig_w = 720, a->sig_h = pal ? 576 : 480, a->sig_fps = pal ? 25 : 30;
            else
                a->sig_w = a->sig_h = a->sig_fps = 0;
            pthread_mutex_unlock(&a->lock);

            const char *mode = sr ? "" : mode_text(st.mode);
            if (streaming)
                set_state(a, APP_STREAMING, "%s%s%s", mode, *mode ? " \xe2\x80\x94 " : "",
                          pal ? "DV PAL 720x576" : "DV NTSC 720x480");
            else if (!sr && st.has_tape == 0 && st.mode != FWDV_MODE_CAMERA)
                set_state(a, APP_NO_SIGNAL, "No tape in the %s", a->name);
            else
                set_state(a, APP_NO_SIGNAL, "%s%sPress Play to see the tape", mode, *mode ? ". " : "");

            /* stop at end of tape: the picture has ended, or the deck says it stopped */
            int tape_stopped = !sr && (st.mode == FWDV_MODE_STOPPED || st.mode == FWDV_MODE_PAUSED);
            if (recording && a->auto_stop && (tape_stopped || !streaming)) {
                if (!stopped_since)
                    stopped_since = now;
                else if (now - stopped_since >= AUTO_STOP_MS - 500)
                    a->auto_stop_due = 1;
            } else {
                stopped_since = 0;
            }
        }
        if (lost)
            set_state(a, APP_SEARCHING, "The DV device was disconnected");
        close_device(a);
    }
    return NULL;
}

/* ---- public: lifecycle and state ---- */

#if !defined(__APPLE__) || defined(LEGACY_MAC)
static void output_cb(int16_t *out, int frames, void *user) { app_audio_render(user, out, frames); }
#endif

app_t *app_start(void)
{
    app_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    pthread_mutex_init(&a->lock, NULL);
    pthread_cond_init(&a->cv, NULL);
    pthread_mutex_init(&a->dev_lock, NULL);
    pthread_mutex_init(&a->data_lock, NULL);
    pthread_mutex_init(&a->frame_lock, NULL);
    a->monitor = 1;
    a->preview = 1;
    a->ring.frames = a->ring_frames;
    a->ring.size = RING_FRAMES;
    a->fw = 720;
    a->fh = 480;
    a->rgb = calloc((size_t)a->fw * a->fh, 4);
    for (int i = 0; i < FRAME_QUEUE; i++)
        a->q[i] = calloc((size_t)a->fw * a->fh, 4);
    a->period_us = 1001000 / 30;
#if !defined(__APPLE__) || defined(LEGACY_MAC)
    a->out = sa_output_start(AUDIO_RATE, output_cb, a);
    if (!a->out)
        snprintf(a->problem, sizeof(a->problem), "No audio output device for monitoring");
#endif
    if (pthread_create(&a->worker, NULL, worker, a)) {
        free(a);
        return NULL;
    }
    return a;
}

static int finish_recording(sink_t *rec, FILE *f, decoder_t *enc);

void app_close(app_t *a)
{
    if (!a)
        return;
    pthread_mutex_lock(&a->data_lock);
    sink_t *rec = a->rec;
    FILE *f = a->rec_file;
    decoder_t *enc = a->enc;
    a->rec = NULL;
    a->rec_file = NULL;
    a->enc = NULL;
    pthread_mutex_unlock(&a->data_lock);
    if (rec || enc)
        finish_recording(rec, f, enc);
    pthread_mutex_lock(&a->lock);
    a->quit = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->lock);
    pthread_join(a->worker, NULL);
#if !defined(__APPLE__) || defined(LEGACY_MAC)
    sa_output_stop(a->out);
#endif
    free(a->rgb);
    for (int i = 0; i < FRAME_QUEUE; i++)
        free(a->q[i]);
    free(a);
}

void app_retry(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->retry = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->lock);
}

app_state_t app_state(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    app_state_t s = a->state;
    pthread_mutex_unlock(&a->lock);
    return s;
}

void app_status(app_t *a, char *out, int len)
{
    pthread_mutex_lock(&a->lock);
    if (a->problem[0])
        snprintf(out, (size_t)len, "%s \xe2\x80\x94 %s", a->status, a->problem);
    else
        snprintf(out, (size_t)len, "%s", a->status);
    pthread_mutex_unlock(&a->lock);
}

void app_device_name(app_t *a, char *out, int len)
{
    pthread_mutex_lock(&a->lock);
    snprintf(out, (size_t)len, "%s", a->name);
    pthread_mutex_unlock(&a->lock);
}

int app_signal(app_t *a, int *w, int *h, int *fps)
{
    pthread_mutex_lock(&a->lock);
    *w = a->sig_w, *h = a->sig_h, *fps = a->sig_fps;
    pthread_mutex_unlock(&a->lock);
    return *w != 0;
}

/* ---- tape ---- */

int app_tape(app_t *a, fwdv_cmd_t cmd)
{
    pthread_mutex_lock(&a->dev_lock);
    int r = a->dev ? fwdv_transport(a->dev, cmd) : FWDV_ERR_NO_DEVICE;
    pthread_mutex_unlock(&a->dev_lock);
    return r;
}

int app_tape_status(app_t *a, fwdv_status_t *st)
{
    pthread_mutex_lock(&a->lock);
    int ok = a->have_tape_status;
    if (ok)
        *st = a->tape;
    pthread_mutex_unlock(&a->lock);
    return ok ? 0 : -1;
}

/* ---- picture ---- */

void app_set_preview(app_t *a, int on)
{
    a->preview = on != 0;
    app_retry(a); /* the worker starts or stops the picture's decoder */
}

int app_preview(app_t *a) { return a->preview; }

void app_frame_size(app_t *a, int *w, int *h)
{
    pthread_mutex_lock(&a->frame_lock);
    *w = a->fw, *h = a->fh;
    pthread_mutex_unlock(&a->frame_lock);
}

int app_get_frame(app_t *a, uint32_t *buf, int w, int h, uint64_t *seq)
{
    int copied = 0;
    pthread_mutex_lock(&a->frame_lock);
    advance_frame(a);
    if (a->seq != *seq && w == a->fw && h == a->fh) {
        memcpy(buf, a->rgb, (size_t)w * h * 4);
        *seq = a->seq;
        copied = 1;
    }
    pthread_mutex_unlock(&a->frame_lock);
    return copied;
}

double app_aspect(app_t *a) { return a->wide ? 16.0 / 9 : 4.0 / 3; }

/* ---- sound ---- */

void app_peaks(app_t *a, double db[2])
{
    for (int c = 0; c < 2; c++) {
        int p = __atomic_exchange_n(&a->peak[c], 0, __ATOMIC_RELAXED);
        db[c] = p > 0 ? 20 * log10(p / 32768.0) : -60;
        if (db[c] < -60)
            db[c] = -60;
    }
}

void app_set_monitor(app_t *a, int on) { a->monitor = on; }

/* 150 ms behind the decoder (see monitor_ring.h) */
static int render(app_t *a, int16_t *out, float *L, float *R, int frames)
{
    return monitor_ring_read(&a->ring, AUDIO_RATE * 150 / 1000, out, L, R, frames, a->monitor ? 1.0f : 0.0f);
}

int app_audio_render(app_t *a, int16_t *out, int frames) { return render(a, out, NULL, NULL, frames); }
int app_audio_render_float(app_t *a, float *left, float *right, int frames)
{
    return render(a, NULL, left, right, frames);
}

/* ---- files ---- */

FILE *app_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8))
        return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

static int make_dir(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024))
        return -1;
    return CreateDirectoryW(wp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 || access(path, F_OK) == 0 ? 0 : -1;
#endif
}

void app_timestamped_path(const char *folder, const char *ext, char *out, int len)
{
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d at %H.%M.%S", &tm);
    size_t fl = strlen(folder);
    int sep = fl && folder[fl - 1] != '/' && folder[fl - 1] != '\\';
    snprintf(out, (size_t)len, "%s%s" APP_NAME " %s.%s", folder, sep ? (DIR_SEP == '/' ? "/" : "\\") : "", stamp,
             ext);
}

void app_default_folder(char *out, int len)
{
#ifdef _WIN32
    static const GUID videos = { 0x18989b1d, 0x99b5, 0x455b, { 0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc } };
    PWSTR w = NULL;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&videos, 0, NULL, &w)))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, len, NULL, NULL);
    CoTaskMemFree(w);
    if (!out[0]) {
        const char *p = getenv("USERPROFILE");
        snprintf(out, (size_t)len, "%s", p ? p : ".");
    }
#else
    const char *home = getenv("HOME");
#ifdef __APPLE__
    snprintf(out, (size_t)len, "%s/Movies", home ? home : ".");
#else
    char cfg[1024], line[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(cfg, sizeof(cfg), "%s/user-dirs.dirs", xdg);
    else
        snprintf(cfg, sizeof(cfg), "%s/.config/user-dirs.dirs", home ? home : "");
    snprintf(out, (size_t)len, "%s/Videos", home ? home : ".");
    FILE *f = fopen(cfg, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *v = strstr(line, "XDG_VIDEOS_DIR=\"");
            if (v != line)
                continue;
            v += 16;
            char *end = strchr(v, '"');
            if (end)
                *end = 0;
            if (!strncmp(v, "$HOME", 5))
                snprintf(out, (size_t)len, "%s%s", home ? home : "", v + 5);
            else
                snprintf(out, (size_t)len, "%s", v);
        }
        fclose(f);
    }
#endif
    if (access(out, F_OK) != 0)
        snprintf(out, (size_t)len, "%s", home ? home : ".");
#endif
}

/* ---- settings ---- */

static void settings_path(char *out, size_t len)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s\\" APP_NAME, base ? base : ".");
    make_dir(dir);
    snprintf(out, len, "%s\\settings.ini", dir);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char base[1024], dir[1100];
    if (xdg && *xdg)
        snprintf(base, sizeof(base), "%s", xdg);
    else
        snprintf(base, sizeof(base), "%s/.config", home ? home : ".");
    make_dir(base);
    snprintf(dir, sizeof(dir), "%s/FireWireRecorder", base);
    make_dir(dir);
    snprintf(out, len, "%s/settings.ini", dir);
#endif
}

void app_setting_str(const char *key, char *out, int len, const char *def)
{
    char path[1200], line[1200];
    size_t kl = strlen(key);
    snprintf(out, (size_t)len, "%s", def ? def : "");
    settings_path(path, sizeof(path));
    FILE *f = app_fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) || line[kl] != '=')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, (size_t)len, "%s", line + kl + 1);
    }
    fclose(f);
}

int app_setting_int(const char *key, int def)
{
    char v[32];
    app_setting_str(key, v, sizeof(v), "");
    return v[0] ? atoi(v) : def;
}

void app_set_setting_str(const char *key, const char *v)
{
    char path[1200], line[1200];
    size_t kl = strlen(key), used = 0, cap = 4096;
    char *all = malloc(cap);
    if (!all)
        return;
    all[0] = 0;
    settings_path(path, sizeof(path));
    FILE *f = app_fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, key, kl) && line[kl] == '=')
                continue;
            size_t n = strlen(line);
            if (used + n + 1 > cap) {
                char *b = realloc(all, cap *= 2);
                if (!b)
                    break;
                all = b;
            }
            memcpy(all + used, line, n + 1);
            used += n;
        }
        fclose(f);
    }
    if ((f = app_fopen(path, "w"))) {
        fputs(all, f);
        fprintf(f, "%s=%s\n", key, v);
        fclose(f);
    }
    free(all);
}

void app_set_setting_int(const char *key, int v)
{
    char s[32];
    snprintf(s, sizeof(s), "%d", v);
    app_set_setting_str(key, s);
}

/* ---- recording ---- */

static int write_file(void *ctx, const uint8_t *p, size_t n) { return fwrite(p, 1, n, ctx) == n ? 0 : -1; }

/* MPEG-2 for DVD (interlaced as DV is, bottom field first) and H.264 with x264
 * (deinterlaced, for computers and phones); the DV's 4:3 or 16:9 carries over. */
static decoder_t *start_encoder(int format, int pal, const char *path, char *err, int errlen)
{
    if (format == APP_FMT_MPEG2) {
        const char *args[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-f", "dv", "-i", "pipe:0",
                               "-map", "0:v:0", "-map", "0:a:0?", "-c:v", "mpeg2video", "-pix_fmt", "yuv420p",
                               "-b:v", "8000k", "-maxrate", "9000k", "-bufsize", "1835k", "-g", pal ? "15" : "18",
                               "-bf", "2", "-flags", "+ilme+ildct", "-alternate_scan", "1",
                               "-c:a", "mp2", "-b:a", "224k", "-ar", "48000", "-ac", "2",
                               "-f", "dvd", "-y", path, NULL };
        return dec_start_queue(args, 0, NULL, NULL, REC_QUEUE, err, errlen);
    }
    const char *args[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-f", "dv", "-i", "pipe:0",
                           "-map", "0:v:0", "-map", "0:a:0?", "-vf", "yadif=mode=send_frame:parity=auto",
                           "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p",
                           "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2",
                           "-movflags", "+faststart", "-f", "mp4", "-y", path, NULL };
    return dec_start_queue(args, 0, NULL, NULL, REC_QUEUE, err, errlen);
}

int app_record_start(app_t *a, const char *folder, int format, char *path, int pathlen, char *err, int errlen)
{
    if (app_state(a) != APP_STREAMING) {
        snprintf(err, (size_t)errlen, "There is no picture to record yet. Press Play on the tape first.");
        return -1;
    }
    if (format < 0 || format >= APP_FORMATS)
        format = APP_FMT_DV;
    make_dir(folder);
    app_timestamped_path(folder, app_format_ext[format], path, pathlen);
    sink_t *s = NULL;
    FILE *f = NULL;
    decoder_t *enc = NULL;
    if (format == APP_FMT_DV) {
        if (!(f = app_fopen(path, "wb"))) {
            snprintf(err, (size_t)errlen, "Cannot create %s", path);
            return -1;
        }
        if (!(s = sink_start(REC_QUEUE, write_file, f))) {
            fclose(f);
            snprintf(err, (size_t)errlen, "out of memory");
            return -1;
        }
    } else {
        char e[300] = "";
        if (!(enc = start_encoder(format, a->pal, path, e, sizeof(e)))) {
            snprintf(err, (size_t)errlen, "Cannot start the encoder: %s", e);
            return -1;
        }
    }
    pthread_mutex_lock(&a->data_lock);
    a->rec = s;
    a->rec_file = f;
    a->enc = enc;
    a->rec_format = format;
    a->rec_frames = 0;
    a->rec_pal = a->pal;
    a->auto_stop_due = 0;
    snprintf(a->rec_path, sizeof(a->rec_path), "%s", path);
    pthread_mutex_unlock(&a->data_lock);
    return 0;
}

static int finish_recording(sink_t *rec, FILE *f, decoder_t *enc)
{
    int ok = 1;
    if (rec) {
        uint64_t dropped = sink_dropped(rec);
        ok &= sink_stop(rec, 1) == 0;
        if (dropped)
            fprintf(stderr, APP_NAME ": %llu bytes were dropped because the disk was too slow\n",
                    (unsigned long long)dropped);
    }
    if (f)
        ok &= fclose(f) == 0;
    if (enc) {
        uint64_t dropped = dec_dropped(enc);
        ok &= dec_finish(enc) == 0;
        if (dropped)
            fprintf(stderr, APP_NAME ": %llu bytes were dropped because the encoder was too slow\n",
                    (unsigned long long)dropped);
    }
    return ok;
}

typedef struct {
    sink_t *rec;
    FILE *f;
    decoder_t *enc;
    app_done_cb done;
    void *user;
    char path[1024];
} stop_job_t;

static void *stop_thread(void *arg)
{
    stop_job_t *j = arg;
    int ok = finish_recording(j->rec, j->f, j->enc);
    if (j->done)
        j->done(j->user, ok, j->path);
    free(j);
    return NULL;
}

void app_record_stop(app_t *a, app_done_cb done, void *user)
{
    pthread_mutex_lock(&a->data_lock);
    sink_t *rec = a->rec;
    FILE *f = a->rec_file;
    decoder_t *enc = a->enc;
    a->rec = NULL;
    a->rec_file = NULL;
    a->enc = NULL;
    a->auto_stop_due = 0;
    pthread_mutex_unlock(&a->data_lock);
    if (!rec && !enc)
        return;
    stop_job_t *j = calloc(1, sizeof(*j));
    if (!j) {
        finish_recording(rec, f, enc);
        return;
    }
    j->rec = rec;
    j->f = f;
    j->enc = enc;
    j->done = done;
    j->user = user;
    snprintf(j->path, sizeof(j->path), "%s", a->rec_path);
    pthread_t t;
    if (pthread_create(&t, NULL, stop_thread, j) == 0)
        pthread_detach(t);
    else
        stop_thread(j);
}

int app_is_recording(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    int r = a->rec || a->enc;
    pthread_mutex_unlock(&a->data_lock);
    return r;
}

double app_record_seconds(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    double s = (a->rec || a->enc) ? a->rec_frames * (a->rec_pal ? 1 / 25.0 : 1001 / 30000.0) : 0;
    pthread_mutex_unlock(&a->data_lock);
    return s;
}

static uint64_t file_size(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024];
    struct _stat64 st;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024) || _wstat64(wp, &st))
        return 0;
    return (uint64_t)st.st_size;
#else
    struct stat st;
    return stat(path, &st) ? 0 : (uint64_t)st.st_size;
#endif
}

uint64_t app_record_bytes(app_t *a)
{
    char path[1024];
    pthread_mutex_lock(&a->data_lock);
    int rec = a->rec != NULL, enc = a->enc != NULL;
    uint64_t b = rec ? sink_written(a->rec) : 0;
    snprintf(path, sizeof(path), "%s", a->rec_path);
    pthread_mutex_unlock(&a->data_lock);
    return enc ? file_size(path) : b;
}

void app_set_auto_stop(app_t *a, int on) { a->auto_stop = on != 0; }

int app_auto_stop_due(app_t *a) { return a->auto_stop_due; }


/* ---- snapshot: minimal PNG writer (stored deflate blocks) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uint32_t c = crc32_update(crc32_update(0, (const uint8_t *)type, 4), data, len);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = app_fopen(path, "wb");
    if (!f)
        return -1;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0; /* 8-bit RGB */
    png_chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (size_t)h * (1 + (size_t)w * 3), blocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(zlen), *raw = malloc(raw_len);
    if (!z || !raw) {
        free(z), free(raw), fclose(f);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (1 + (size_t)w * 3)] = 0; /* filter: none */
        memcpy(raw + (size_t)y * (1 + (size_t)w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    uint8_t *p = z;
    *p++ = 0x78, *p++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw_len;) {
        uint32_t n = (uint32_t)(raw_len - off > 65535 ? 65535 : raw_len - off);
        *p++ = off + n == raw_len;
        *p++ = (uint8_t)n, *p++ = (uint8_t)(n >> 8), *p++ = (uint8_t)~n, *p++ = (uint8_t)(~n >> 8);
        memcpy(p, raw + off, n);
        for (uint32_t i = 0; i < n; i++) {
            s1 = (s1 + raw[off + i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        p += n;
        off += n;
    }
    be32(p, s2 << 16 | s1);
    p += 4;
    png_chunk(f, "IDAT", z, (uint32_t)(p - z));
    png_chunk(f, "IEND", NULL, 0);
    free(z);
    free(raw);
    return fclose(f) ? -1 : 0;
}

int app_snapshot(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen)
{
    if (app_state(a) != APP_STREAMING) {
        snprintf(err, (size_t)errlen, "There is no picture to save yet.");
        return -1;
    }
    if (!a->preview) {
        snprintf(err, (size_t)errlen, "Turn the preview on to save a snapshot.");
        return -1;
    }
    int w, h;
    app_frame_size(a, &w, &h);
    /* resample to square pixels: 720 BT.601 samples span the 4:3 (or 16:9) picture */
    const int oh = h, ow = (int)(h * app_aspect(a) / 2 + 0.5) * 2;
    uint32_t *src = malloc((size_t)w * h * 4);
    uint8_t *dst = malloc((size_t)ow * oh * 3);
    uint64_t seq = (uint64_t)-1;
    if (!src || !dst || !app_get_frame(a, src, w, h, &seq)) {
        free(src), free(dst);
        snprintf(err, (size_t)errlen, "No picture to save");
        return -1;
    }
    for (int y = 0; y < oh; y++) {
        const uint32_t *row = src + (size_t)y * w;
        for (int x = 0; x < ow; x++) {
            double fx = (x + 0.5) * w / ow - 0.5;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < w ? x0 + 1 : x0;
            double t = fx - x0;
            if (t < 0)
                t = 0;
            for (int c = 0; c < 3; c++) {
                int sh = 16 - 8 * c;
                double v = ((row[x0] >> sh) & 0xff) * (1 - t) + ((row[x1] >> sh) & 0xff) * t;
                dst[((size_t)y * ow + x) * 3 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
    make_dir(folder);
    app_timestamped_path(folder, "png", path, pathlen);
    int r = write_png(path, dst, ow, oh);
    if (r)
        snprintf(err, (size_t)errlen, "Cannot write %s", path);
    free(src);
    free(dst);
    return r;
}
