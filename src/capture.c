/*
 * fwdv-capture: record DV from a FireWire camcorder or deck, frame for frame
 * as it is on the tape (a raw .dv file); HDV comes out as its MPEG-2 transport stream (.m2t).
 *
 *   fwdv-capture -o tape.dv --play --until-stop    play the tape and record it to the end
 *   fwdv-capture -o - | ffplay -f dv -             watch it live
 *   fwdv-capture --rewind                          tape commands on their own
 */
#include "fwdv.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

static volatile int g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static FILE *g_out;
static volatile int g_write_error;
static volatile long g_frames;
static volatile time_t g_last_frame;
static volatile long g_ts_bytes;

static void on_frame(const uint8_t *frame, size_t len, void *user)
{
    (void)user;
    if (g_out && !g_write_error && fwrite(frame, 1, len, g_out) != len)
        g_write_error = 1;
    g_frames++;
    g_last_frame = time(NULL);
}

/* HDV: the MPEG-2 transport stream as it comes (a .m2t file) */
static void on_ts(const uint8_t *ts, size_t len, void *user)
{
    (void)user;
    if (g_out && !g_write_error && fwrite(ts, 1, len, g_out) != len)
        g_write_error = 1;
    g_ts_bytes += (long)len;
    g_frames++;
    g_last_frame = time(NULL);
}

/* Records a raw .dv file onto the camcorder's tape: the camcorder must be in VCR mode with a
 * recordable tape, and records from the tape's current position. */
static int send_file(fwdv_t *d, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    static uint8_t buf[DV_FRAME_625];
    if (fread(buf, 1, DV_FRAME_525, f) != DV_FRAME_525) {
        fprintf(stderr, "fwdv-capture: %s is not a DV file\n", path);
        fclose(f);
        return 1;
    }
    int pal = buf[3] & 0x80; /* DSF in the header block */
    size_t len = pal ? DV_FRAME_625 : DV_FRAME_525;
    if (pal && fread(buf + DV_FRAME_525, 1, len - DV_FRAME_525, f) != len - DV_FRAME_525) {
        fprintf(stderr, "fwdv-capture: %s ends in the middle of a frame\n", path);
        fclose(f);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    int r = fwdv_send_start(d, pal);
    if (r) {
        fprintf(stderr, "fwdv-capture: cannot send: %s\n", fwdv_strerror(r));
        fclose(f);
        return 1;
    }
    long n = 0;
    int recording = 0;
    do {
        if ((r = fwdv_send_frame(d, buf, len, &g_stop)))
            break;
        n++;
        if (n == 15 && !recording) { /* the stream is running: tell the tape to record it */
            if ((r = fwdv_transport(d, FWDV_RECORD)))
                fprintf(stderr, "fwdv-capture: record: %s\n", fwdv_strerror(r));
            recording = 1;
        }
        if (n % 30 == 0)
            fprintf(stderr, "\r%ld frames sent", n);
    } while (!g_stop && fread(buf, 1, len, f) == len);
    fclose(f);
    if (!recording && !r)
        fwdv_transport(d, FWDV_RECORD);
    fwdv_send_stop(d, !g_stop);
    fwdv_transport(d, FWDV_STOP);
    fprintf(stderr, "\nfwdv-capture: sent %ld frames%s\n", n, r ? " (stopped early)" : "");
    return r ? 1 : 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "      --send FILE     record a raw DV file onto the tape (camcorder in VCR mode, from the current position)\n"
            "  -o, --output FILE   raw DV (HDV: transport stream) file, - for stdout (without -o: only tape commands)\n"
            "  -t, --time SECONDS  stop recording after this many seconds\n"
            "      --play          start the tape playing first\n"
            "      --until-stop    stop when the tape stops or the picture ends\n"
            "      --stop          stop the tape          --pause  pause it\n"
            "      --rewind        rewind the tape        --ffwd   fast forward it\n"
            "      --info          print the device and tape status and exit\n"
            "  -v, --verbose       more logging (twice: AV/C traces)\n",
            argv0);
}

static const char *mode_name(fwdv_mode_t m)
{
    static const char *const n[] = { "unknown", "stopped", "playing", "paused", "rewinding",
                                      "fast forwarding", "playing in reverse", "recording", "camera" };
    return (unsigned)m < sizeof(n) / sizeof(n[0]) ? n[m] : "unknown";
}

enum { OPT_PLAY = 256, OPT_UNTIL, OPT_STOP, OPT_PAUSE, OPT_REW, OPT_FF, OPT_INFO, OPT_SEND };

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "output", required_argument, 0, 'o' }, { "time", required_argument, 0, 't' },
        { "play", no_argument, 0, OPT_PLAY },    { "until-stop", no_argument, 0, OPT_UNTIL },
        { "stop", no_argument, 0, OPT_STOP },    { "pause", no_argument, 0, OPT_PAUSE },
        { "rewind", no_argument, 0, OPT_REW },   { "ffwd", no_argument, 0, OPT_FF },
        { "send", required_argument, 0, OPT_SEND }, { "info", no_argument, 0, OPT_INFO },    { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },         { 0, 0, 0, 0 },
    };
    const char *outpath = NULL, *sendpath = NULL;
    double seconds = 0;
    int play = 0, until = 0, info = 0, verbose = 0, c;
    int cmd = -1;
    while ((c = getopt_long(argc, argv, "o:t:vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'o': outpath = optarg; break;
        case 't': seconds = atof(optarg); break;
        case OPT_PLAY: play = 1; break;
        case OPT_UNTIL: until = 1; break;
        case OPT_STOP: cmd = FWDV_STOP; break;
        case OPT_PAUSE: cmd = FWDV_PAUSE; break;
        case OPT_REW: cmd = FWDV_REWIND; break;
        case OPT_FF: cmd = FWDV_FFWD; break;
        case OPT_INFO: info = 1; break;
        case OPT_SEND: sendpath = optarg; break;
        case 'v': verbose++; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    fwdv_set_verbose(verbose);

    fwdv_t *d;
    int r = fwdv_open(&d);
    if (r) {
        fprintf(stderr, "fwdv-capture: %s\n", fwdv_strerror(r));
        return 1;
    }
    fprintf(stderr, "fwdv-capture: %s\n", fwdv_name(d));

    if (sendpath) { /* record a raw DV file onto the tape, from where it is now */
        int rc = send_file(d, sendpath);
        fwdv_close(d);
        return rc;
    }

    fwdv_status_t st;
    if (info || (!outpath && cmd < 0 && !play)) {
        if (fwdv_status(d, &st) == 0) {
            fprintf(stderr, "tape: %s, %s", mode_name(st.mode),
                    st.has_tape < 0 ? "cassette unknown" : st.has_tape ? "cassette in" : "no cassette");
            if (st.tc_valid)
                fprintf(stderr, ", timecode %02d:%02d:%02d.%02d", st.tc_h, st.tc_m, st.tc_s, st.tc_f);
            fprintf(stderr, "\n");
        }
        fwdv_close(d);
        return 0;
    }
    if (cmd >= 0) {
        r = fwdv_transport(d, (fwdv_cmd_t)cmd);
        if (r)
            fprintf(stderr, "fwdv-capture: %s\n", fwdv_strerror(r));
        if (!outpath) {
            fwdv_close(d);
            return r ? 1 : 0;
        }
    }
    if (!outpath) {
        if (play && (r = fwdv_transport(d, FWDV_PLAY)))
            fprintf(stderr, "fwdv-capture: %s\n", fwdv_strerror(r));
        fwdv_close(d);
        return r ? 1 : 0;
    }

    if (!strcmp(outpath, "-")) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        g_out = stdout;
    } else if (!(g_out = fopen(outpath, "wb"))) {
        perror(outpath);
        fwdv_close(d);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fwdv_set_ts_handler(on_ts, NULL);
    if ((r = fwdv_start(d, on_frame, NULL))) {
        fprintf(stderr, "fwdv-capture: cannot receive: %s\n", fwdv_strerror(r));
        fwdv_close(d);
        return 1;
    }
    if (play && (r = fwdv_transport(d, FWDV_PLAY)))
        fprintf(stderr, "fwdv-capture: play: %s\n", fwdv_strerror(r));

    time_t t0 = time(NULL);
    g_last_frame = t0;
    int stopped_polls = 0;
    while (!g_stop && !g_write_error) {
        sleep_ms(500);
        time_t now = time(NULL);
        if (!g_frames)
            t0 = now; /* the tape takes a moment to start: count from the first frame */
        if (seconds > 0 && g_frames && difftime(now, t0) >= seconds)
            break;
        if (until) {
            if (fwdv_status(d, &st) == 0 && g_frames &&
                (st.mode == FWDV_MODE_STOPPED || st.mode == FWDV_MODE_PAUSED))
                stopped_polls++;
            else
                stopped_polls = 0;
            if (stopped_polls >= 4 || (g_frames && difftime(now, g_last_frame) >= 5))
                break;
        }
        if (!fwdv_alive(d)) {
            fprintf(stderr, "\nfwdv-capture: %s\n", fwdv_strerror(FWDV_ERR_GONE));
            break;
        }
        if (g_out != stdout)
            fprintf(stderr, "\r%ld frames, %.1f MB", g_frames,
                    g_ts_bytes ? g_ts_bytes / 1e6 : g_frames * (double)DV_FRAME_525 / 1e6);
    }
    fwdv_stop(d);
    if (play) {
        /* a deck still threading the tape can let a stop go by: check, and send it again */
        for (int i = 0; i < 6; i++) {
            if (i % 2 == 0)
                fwdv_transport(d, FWDV_STOP);
            sleep_ms(500);
            if (fwdv_status(d, &st) == 0 && st.mode != FWDV_MODE_PLAYING && st.mode != FWDV_MODE_PAUSED)
                break;
        }
    }
    fwdv_close(d);
    if (g_out != stdout && fclose(g_out))
        g_write_error = 1;
    if (g_ts_bytes)
        fprintf(stderr, "\nfwdv-capture: HDV, %.1f MB%s\n", g_ts_bytes / 1e6, g_write_error ? ", write error" : "");
    else
    fprintf(stderr, "\nfwdv-capture: %ld frames%s\n", g_frames, g_write_error ? ", write error" : "");
    return g_write_error ? 1 : 0;
}
