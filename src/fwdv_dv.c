/* DV frame assembly from CIP packets, and the AV/C command and status bytes,
 * shared by the macOS and Linux backends (Windows gets frames and transport
 * control from its DV driver). See fwdv.h. */
#include "fwdv.h"

#include <stdlib.h>
#include <string.h>

int fwdv_verbose;

static fwdv_ts_cb g_ts_cb;
static void *g_ts_user;

void fwdv_set_ts_handler(fwdv_ts_cb cb, void *user)
{
    g_ts_cb = cb;
    g_ts_user = user;
}

void fwdv_set_verbose(int level) { fwdv_verbose = level; }

const char *fwdv_strerror(int r)
{
    switch (r) {
    case FWDV_OK: return "OK";
    case FWDV_ERR_NO_DEVICE: return "no DV device on FireWire";
    case FWDV_ERR_BUSY: return "the device is in use by another program";
    case FWDV_ERR_ACCESS: return "no permission to use the FireWire device";
    case FWDV_ERR_IO: return "FireWire I/O error";
    case FWDV_ERR_GONE: return "the device was disconnected";
    case FWDV_ERR_UNSUPPORTED: return "the device does not support this";
    default: return "unknown error";
    }
}

/*
 * IEC 61883-2: each packet is a two-quadlet CIP header and, in a data packet,
 * six 80-byte DIF blocks (empty packets keep the rate). A frame starts at the
 * header block of DIF sequence 0 and has 10 (525/60) or 12 (625/50) sequences
 * of 150 blocks. Frames with blocks missing are dropped: a .dv file is a run of
 * whole frames.
 */
void dv_assemble(dv_assembler_t *a, const uint8_t *p, size_t n)
{
    if (n < 8)
        return;
    if ((p[4] & 0x3f) == 0x20) { /* FMT 0x20: MPEG2-TS (HDV), 188-byte packets, usually with a 4-byte time stamp */
        if (!g_ts_cb)
            return;
        /* source packets are 192 bytes (4-byte time stamp first) or 188; the payload size says which */
        size_t sph = p[2] & 0x04 ? 4 : 0, pl = n - 8;
        if (pl && pl % 192 == 0)
            sph = 4;
        else if (pl && pl % 188 == 0)
            sph = 0;
        size_t stride = 188 + sph;
        const uint8_t *q = p + 8;
        for (size_t o = 0; o + stride <= n - 8; o += stride) {
            if (q[o + sph] != 0x47) /* not a TS packet: padding or a bus glitch */
                continue;
            memcpy(a->ts + a->ts_len, q + o + sph, 188);
            a->ts_len += 188;
            if (a->ts_len + 188 > (int)sizeof(a->ts)) {
                g_ts_cb(a->ts, (size_t)a->ts_len, g_ts_user);
                a->frames++;
                a->ts_len = 0;
            }
        }
        return;
    }
    /* FMT 0 is DV; DBS is 120 quadlets */
    if ((p[4] & 0x3f) != 0 || p[1] != 0x78)
        return;
    p += 8, n -= 8;
    for (size_t b = 0; b + 80 <= n; b += 80) {
        const uint8_t *blk = p + b;
        int sct = blk[0] >> 5, dseq = blk[1] >> 4, dbn = blk[2];
        if (sct == 0 && dseq == 0 && dbn == 0) {
            if (a->started && a->len) {
                int pal = a->buf[3] & 0x80; /* DSF in the header block */
                int want = pal ? DV_FRAME_625 : DV_FRAME_525;
                if (a->len == want) {
                    a->frames++;
                    if (a->cb)
                        a->cb(a->buf, (size_t)a->len, a->user);
                } else {
                    a->dropped++;
                }
            }
            a->started = 1;
            a->len = 0;
        }
        if (!a->started)
            continue;
        if (a->len + 80 > (int)sizeof(a->buf)) { /* runaway: wait for the next frame start */
            a->started = 0;
            a->dropped++;
            continue;
        }
        memcpy(a->buf + a->len, blk, 80);
        a->len += 80;
    }
}

/* ---- AV/C (IEC 61883-1, AV/C Tape Recorder/Player Subunit 2.1) ---- */

#define AVC_CONTROL 0x00
#define AVC_TAPE 0x20 /* subunit type 4 (tape recorder/player), id 0 */

int avc_cmd_bytes(fwdv_cmd_t cmd, uint8_t out[8])
{
    out[0] = AVC_CONTROL;
    out[1] = AVC_TAPE;
    switch (cmd) {
    case FWDV_PLAY: out[2] = 0xc3, out[3] = 0x75; break;         /* PLAY forward */
    case FWDV_PAUSE: out[2] = 0xc3, out[3] = 0x7d; break;        /* PLAY forward pause */
    case FWDV_PLAY_REVERSE: out[2] = 0xc3, out[3] = 0x65; break; /* PLAY reverse */
    case FWDV_STEP_FWD: out[2] = 0xc3, out[3] = 0x30; break;     /* next frame */
    case FWDV_STEP_BACK: out[2] = 0xc3, out[3] = 0x40; break;    /* previous frame */
    case FWDV_STOP: out[2] = 0xc4, out[3] = 0x60; break;         /* WIND stop */
    case FWDV_REWIND: out[2] = 0xc4, out[3] = 0x65; break;       /* WIND rewind */
    case FWDV_FFWD: out[2] = 0xc4, out[3] = 0x75; break;         /* WIND fast forward */
    case FWDV_RECORD: out[2] = 0xc2, out[3] = 0x75; break;       /* RECORD */
    case FWDV_RECORD_PAUSE: out[2] = 0xc2, out[3] = 0x7d; break; /* RECORD pause */
    default: return 0;
    }
    return 4;
}

/* response to TRANSPORT STATE (01 20 d0 7f): the opcode and operand of the current mode */
void avc_parse_transport(const uint8_t *r, int n, fwdv_status_t *st)
{
    st->mode = FWDV_MODE_UNKNOWN;
    if (n < 4 || r[0] != 0x0c)
        return;
    int op = r[2], v = r[3];
    if (op == 0xc2) {
        st->mode = FWDV_MODE_RECORDING;
    } else if (op == 0xc3) {
        if (v == 0x7d || v == 0x6d)
            st->mode = FWDV_MODE_PAUSED;
        else if ((v >= 0x40 && v <= 0x4f) || v == 0x65 || (v >= 0x50 && v <= 0x5f) || v == 0x6f)
            st->mode = v == 0x40 ? FWDV_MODE_PAUSED : FWDV_MODE_REVERSE;
        else if (v == 0x30)
            st->mode = FWDV_MODE_PAUSED;
        else
            st->mode = FWDV_MODE_PLAYING;
    } else if (op == 0xc4) {
        if (v == 0x65)
            st->mode = FWDV_MODE_REWINDING;
        else if (v == 0x75)
            st->mode = FWDV_MODE_FFWD;
        else
            st->mode = FWDV_MODE_STOPPED;
    }
}

static int bcd(uint8_t v) { return (v >> 4 & 0x0f) * 10 + (v & 0x0f); }

/* response to TIME CODE status (01 20 51 71 ff ff ff ff): frames, seconds, minutes, hours in BCD */
void avc_parse_timecode(const uint8_t *r, int n, fwdv_status_t *st)
{
    st->tc_valid = 0;
    if (n < 8 || r[0] != 0x0c || r[4] == 0xff || r[7] == 0xff)
        return;
    st->tc_f = bcd(r[4] & 0x3f);
    st->tc_s = bcd(r[5] & 0x7f);
    st->tc_m = bcd(r[6] & 0x7f);
    st->tc_h = bcd(r[7] & 0x3f);
    st->tc_valid = st->tc_h < 24 && st->tc_m < 60 && st->tc_s < 60;
}

/* response to MEDIUM INFO (01 20 da 7f 7f): cassette type 0x60 means none */
void avc_parse_medium(const uint8_t *r, int n, fwdv_status_t *st)
{
    st->has_tape = -1;
    if (n >= 5 && r[0] == 0x0c)
        st->has_tape = r[3] != 0x60;
}

/* ---- sending: DV frame -> CIP packets ---- */

#define TICKS_PER_CYCLE 3072.0

void dv_tx_init(dv_tx_t *t, int pal, uint32_t start_cycle)
{
    memset(t, 0, sizeof(*t));
    t->no_syt = getenv("FWDV_SYT") && !strcmp(getenv("FWDV_SYT"), "none");
    t->pal = pal;
    t->npkt = pal ? 300 : 250; /* 6 DIF blocks of 80 bytes each: 1500 or 1800 blocks per frame */
    t->step = pal ? 24576000.0 / 25 / 300 : 24576000.0 * 1001 / 30000 / 250;
    t->cycle = start_cycle;
    t->next_pt = ((double)start_cycle + DV_TX_AHEAD + 1) * TICKS_PER_CYCLE;
}

int dv_tx_packet(dv_tx_t *t, uint8_t *out)
{
    double now = (double)t->cycle++ * TICKS_PER_CYCLE;
    int data = 0;
    if (t->frame) {
        if (t->idx == 0 && t->next_pt < now + (DV_TX_AHEAD - 2) * TICKS_PER_CYCLE)
            t->next_pt = now + (DV_TX_AHEAD + 1) * TICKS_PER_CYCLE; /* late (or just started): re-time */
        data = now + DV_TX_AHEAD * TICKS_PER_CYCLE >= t->next_pt;
    }
    uint32_t syt = 0xffff;
    if (data && !t->no_syt) {
        uint64_t pt = (uint64_t)t->next_pt;
        syt = (uint32_t)((pt / 3072 & 15) << 12 | pt % 3072);
    }
    out[0] = t->sid & 0x3f; /* source node */
    out[1] = 0x78; /* 120 quadlets per data block */
    out[2] = 0;
    out[3] = t->dbc;
    out[4] = 0x80; /* EOH1, FMT 0: DV */
    out[5] = t->pal ? 0x80 : 0x00; /* 50/60 */
    out[6] = (uint8_t)(syt >> 8);
    out[7] = (uint8_t)syt;
    if (!data)
        return 8;
    memcpy(out + 8, t->frame + (size_t)t->idx * 480, 480);
    t->dbc++;
    t->next_pt += t->step;
    if (++t->idx == t->npkt) {
        t->idx = 0;
        t->frame = NULL;
        t->frame_done = 1;
    }
    return 488;
}

#if !defined(__APPLE__) || defined(LEGACY_MAC)
int fwdv_send_start(fwdv_t *d, int pal) { (void)d, (void)pal; return FWDV_ERR_UNSUPPORTED; }
int fwdv_send_frame(fwdv_t *d, const uint8_t *frame, size_t len, volatile int *abort)
{
    (void)d, (void)frame, (void)len, (void)abort;
    return FWDV_ERR_UNSUPPORTED;
}
void fwdv_send_stop(fwdv_t *d, int drain) { (void)d, (void)drain; }
void fwdv_send_stats(fwdv_t *d, uint64_t *queued, uint64_t *sent)
{
    (void)d;
    *queued = *sent = 0;
}
#endif
