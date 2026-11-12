/* Drives the engine without a device: FWDV_REPLAY feeds a raw .dv file.
 *   FWDV_REPLAY=sample.dv build/test_engine OUTDIR
 * Records 4 s in each format, checks the preview switch, saves a snapshot. */
#include "app_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__ /* clock_gettime is 10.12 and later; this runs on 10.9 too */
#include <mach/mach_time.h>
static int mono_clock(struct timespec *ts)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    uint64_t ns = mach_absolute_time() * tb.numer / tb.denom;
    ts->tv_sec = (time_t)(ns / 1000000000u);
    ts->tv_nsec = (long)(ns % 1000000000u);
    return 0;
}
#define clock_gettime(clk, ts) mono_clock(ts)
#endif

static volatile int done, done_ok;
static char done_path[1100];
static void rec_done(void *u, int ok, const char *p)
{
    (void)u;
    snprintf(done_path, sizeof(done_path), "%s", p);
    done_ok = ok;
    done = 1;
}

/* the DV data code: a frame with REC DATE/REC TIME packs in its VAUX, and one with them in the subcode only */
static int test_rec_datetime(void)
{
    static uint8_t f[DV_FRAME_525];
    dv_recdate_t d;
    memset(f, 0xff, sizeof(f));
    if (dv_rec_datetime(f, sizeof(f), &d))
        return fprintf(stderr, "rec date: found one in an empty frame\n"), 1;
    for (int s = 0; s < 10; s++)
        for (int b = 0; b < 150; b++)
            f[(s * 150 + b) * 80] = (uint8_t)(b == 0 ? 0x1f : b < 3 ? 0x3f : b < 6 ? 0x5f : 0x9f); /* SCT */
    uint8_t *ssyb = f + 1 * 80 + 3 + 2 * 8 + 3, *ssyb2 = f + 2 * 80 + 3 + 4 * 8 + 3;
    memcpy(ssyb, "\x62\xff\x09\x12\x98", 5);  /* 9 Dec 1998 */
    memcpy(ssyb2, "\x63\xff\x05\x30\x21", 5); /* 21:30:05 */
    if (!dv_rec_datetime(f, sizeof(f), &d) || d.year != 1998 || d.month != 12 || d.day != 9 || !d.has_time ||
        d.hour != 21 || d.min != 30 || d.sec != 5)
        return fprintf(stderr, "rec date (subcode): %d-%d-%d %d %d:%d:%d\n", d.year, d.month, d.day, d.has_time,
                       d.hour, d.min, d.sec), 1;
    uint8_t *vaux = f + (3 * 150 + 4) * 80 + 3 + 6 * 5; /* sequence 3, VAUX block 2, pack 6 */
    memcpy(vaux, "\x62\xe5\xd4\xe7\x03", 5);      /* 14 Jul 2003 (the top bits of each byte are flags) */
    memcpy(vaux + 5, "\x63\xc0\x07\x42\x15", 5);  /* 15:42:07 */
    if (!dv_rec_datetime(f, sizeof(f), &d) || d.year != 2003 || d.month != 7 || d.day != 14 || d.hour != 15 ||
        d.min != 42 || d.sec != 7)
        return fprintf(stderr, "rec date (VAUX): %d-%d-%d %d:%d:%d\n", d.year, d.month, d.day, d.hour, d.min,
                       d.sec), 1;
    fprintf(stderr, "rec date: ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (test_rec_datetime())
        return 1;
    const char *dir = argc > 1 ? argv[1] : ".";
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    app_t *a = app_start();
    for (int i = 0; i < 50 && app_state(a) != APP_STREAMING; i++)
        usleep(100000);
    if (app_state(a) != APP_STREAMING) {
        fprintf(stderr, "FAIL: not streaming\n");
        return 1;
    }
    /* poll the preview at 60 Hz for 10 s, like the Mac viewer */
    int w, h;
    app_frame_size(a, &w, &h);
    uint32_t *f = calloc((size_t)w * h, 4);
    uint64_t seq = 0;
    int frames = 0, hitches = 0;
    double first = -1, last = -1, worst = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double poll0 = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
    double shown = -1; /* the first real picture (the first frame counted is the cleared one) */
    int shown_frames = 0;
    for (int i = 0; i < 600; i++) {
        if (app_get_frame(a, f, w, h, &seq)) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            double now = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
            if (first < 0)
                first = now;
            else if (now - last > 0.050) {
                hitches++;
                if (getenv("GAPS"))
                    fprintf(stderr, "gap %.0f ms at %.2f s\n", (now - last) * 1000, now);
            }
            if (last >= 0 && now - last > worst)
                worst = now - last;
            last = now;
            frames++;
            if (frames == 2)
                shown = now;
            if (frames >= 2)
                shown_frames++;
        }
        /* poll on a steady 60 Hz grid, as a display link would */
        clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
        double next = poll0 + (i + 1) / 60.0;
        if (next > el)
            usleep((useconds_t)((next - el) * 1e6));
    }
    printf("preview: picture %.2f s after start, then %.2f frames/s, %d gaps over 50 ms (worst %.0f ms incl. the wait for the first picture)\n",
           shown, (shown_frames - 1) / (last - shown), hitches, worst * 1000);
    double db[2];
    app_peaks(a, db);
    printf("frame %dx%d, %d frames, aspect %.3f, peaks %.1f/%.1f dBFS\n", w, h, frames, app_aspect(a),
           db[0], db[1]);
    char path[1100], err[300];
    if (app_snapshot(a, dir, path, sizeof(path), err, sizeof(err)))
        fprintf(stderr, "FAIL snapshot: %s\n", err);
    else
        printf("snapshot %s\n", path);
    for (int fmt = 0; fmt < APP_FORMATS; fmt++) {
        done = 0;
        if (app_record_start(a, dir, fmt, path, sizeof(path), err, sizeof(err))) {
            fprintf(stderr, "FAIL record %s: %s\n", app_format_names[fmt], err);
            return 1;
        }
        sleep(4);
        printf("%s: recorded %.1f s, %.2f MB\n", app_format_names[fmt], app_record_seconds(a),
               app_record_bytes(a) / 1e6);
        app_record_stop(a, rec_done, NULL);
        while (!done)
            usleep(10000);
        printf("saved %s ok=%d\n", done_path, done_ok);
        if (!done_ok)
            return 1;
    }
    /* preview off: no new frames, sound still there */
    app_set_preview(a, 0);
    sleep(2);
    uint64_t s0 = seq;
    int got = 0;
    for (int i = 0; i < 60; i++, usleep(16000))
        got += app_get_frame(a, f, w, h, &seq);
    app_peaks(a, db);
    usleep(500000);
    app_peaks(a, db);
    printf("preview off: %d new frames (seq %llu -> %llu), peaks %.1f/%.1f dBFS\n", got, (unsigned long long)s0,
           (unsigned long long)seq, db[0], db[1]);
    app_set_preview(a, 1);
    sleep(2);
    got = 0;
    for (int i = 0; i < 60; i++, usleep(16000))
        got += app_get_frame(a, f, w, h, &seq);
    printf("preview on again: %d new frames\n", got);
    app_close(a);
    return frames > 0 && done_ok ? 0 : 1;
}
