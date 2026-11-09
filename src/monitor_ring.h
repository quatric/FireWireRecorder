/*
 * The audio monitor's ring buffer: the capture side writes 16-bit stereo
 * frames, the speakers read them a fixed latency behind.
 *
 * Input and output run on different clocks and deliver in different block
 * sizes, so the reader keeps a cushion of `target` frames. When the cushion
 * runs dry the reader waits until it has built up again instead of playing
 * whatever trickles in (which drops out on every block and sounds like
 * stutter), and it follows slow clock drift by skipping or repeating one
 * frame in 512 rather than jumping.
 *
 * Positions are 32-bit and wrap: the ring size must be a power of two, and
 * 32-bit loads and stores are atomic on every CPU this runs on, PowerPC
 * included.
 */
#ifndef MONITOR_RING_H
#define MONITOR_RING_H

#include <stdint.h>

typedef struct {
    int16_t *frames;          /* size * 2 samples */
    uint32_t size;            /* power of two */
    volatile uint32_t w;      /* frames written */
    uint32_t r;               /* frames read (output thread only) */
    volatile int primed;      /* 0: building the cushion, output is silent */
    volatile int reset;       /* set by the input side: restart the reader */
} monitor_ring_t;

static inline void monitor_ring_write(monitor_ring_t *m, const int16_t *s, int n, volatile int *peak)
{
    uint32_t w = __atomic_load_n(&m->w, __ATOMIC_RELAXED), mask = m->size - 1;
    int pl = 0, pr = 0;
    for (int i = 0; i < n; i++, w++) {
        int16_t l = s[2 * i], r = s[2 * i + 1];
        m->frames[(w & mask) * 2] = l;
        m->frames[(w & mask) * 2 + 1] = r;
        if (l < 0) l = (int16_t)(l == -32768 ? 32767 : -l);
        if (r < 0) r = (int16_t)(r == -32768 ? 32767 : -r);
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    __atomic_store_n(&m->w, w, __ATOMIC_RELEASE);
    if (peak) {
        if (pl > peak[0]) peak[0] = pl;
        if (pr > peak[1]) peak[1] = pr;
    }
}

/* Starts the reader over (a new input, or after a long pause): silence until the cushion is back. */
static inline void monitor_ring_restart(monitor_ring_t *m) { m->reset = 1; }

/* Fills n frames, interleaved 16-bit (out16) or planar float (L/R). gain 0 mutes but keeps the
 * reader moving. Returns 1 if anything audible was written. */
static inline int monitor_ring_read(monitor_ring_t *m, uint32_t target, int16_t *out16, float *L, float *R, int n,
                                    float gain)
{
    const uint32_t w = __atomic_load_n(&m->w, __ATOMIC_ACQUIRE), mask = m->size - 1;
    uint32_t r = m->r, avail = w - r;
    int i = 0, any = 0;
    if (target > m->size / 4)
        target = m->size / 4;
    if (m->reset || avail > m->size) { /* restarted, or the writer lapped us */
        m->reset = 0;
        m->primed = 0;
        r = w;
        avail = 0;
    }
    if (!m->primed) {
        if (avail >= target) {
            m->primed = 1;
            r = w - target; /* start exactly the cushion behind */
            avail = target;
        }
    } else if (avail > 4 * target) { /* the output stalled for a while: catch up in one go */
        r = w - target;
        avail = target;
    }
    if (m->primed) {
        for (; i < n; i++) {
            if (r == w) { /* ran dry: wait for a full cushion again */
                m->primed = 0;
                break;
            }
            int16_t l = m->frames[(r & mask) * 2], rr = m->frames[(r & mask) * 2 + 1];
            if (out16) {
                out16[2 * i] = (int16_t)(l * gain);
                out16[2 * i + 1] = (int16_t)(rr * gain);
            } else {
                L[i] = l / 32768.0f * gain;
                R[i] = rr / 32768.0f * gain;
            }
            if (l || rr)
                any = 1;
            r++;
            if ((i & 511) == 511) { /* follow the clocks' drift, one frame at a time */
                avail = w - r;
                if (avail > 2 * target && avail > 1)
                    r++;
                else if (avail < target / 2)
                    r--;
            }
        }
    }
    for (; i < n; i++) {
        if (out16)
            out16[2 * i] = out16[2 * i + 1] = 0;
        else
            L[i] = R[i] = 0;
    }
    m->r = r;
    return any && gain > 0;
}

#endif
