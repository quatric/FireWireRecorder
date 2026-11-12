/*
 * DV camcorders and decks on FireWire (IEEE 1394): finds one, receives its DV
 * stream as whole frames, and drives its tape transport with AV/C commands.
 *
 *   macOS    IOFireWireLib isochronous receive + IOFireWireAVCLib (fwdv_mac.c),
 *            Mac OS X 10.3 and later
 *   Linux    the kernel's firewire-cdev interface, /dev/fw* (fwdv_linux.c)
 *   Windows  the system's DV driver (MSDV) through DirectShow (fwdv_win32.c)
 *
 * A frame is 120000 bytes (525/60, NTSC) or 144000 bytes (625/50, PAL) of DIF
 * blocks, exactly as recorded on the tape; written one after another they make
 * a raw .dv file.
 */
#ifndef FWDV_H
#define FWDV_H

#include <stddef.h>
#include <stdint.h>

#define DV_FRAME_525 120000
#define DV_FRAME_625 144000

typedef struct fwdv fwdv_t;

enum {
    FWDV_OK = 0,
    FWDV_ERR_NO_DEVICE = -1,  /* nothing DV on FireWire */
    FWDV_ERR_BUSY = -2,       /* another program has it */
    FWDV_ERR_ACCESS = -3,     /* no permission (Linux: /dev/fw*) */
    FWDV_ERR_IO = -4,
    FWDV_ERR_GONE = -5,       /* unplugged or switched off */
    FWDV_ERR_UNSUPPORTED = -6 /* the device does not do that command */
};
const char *fwdv_strerror(int r);

/* Called on the receive thread with each complete frame. */
typedef void (*fwdv_frame_cb)(const uint8_t *frame, size_t len, void *user);

/* HDV camcorders send an MPEG-2 transport stream (IEC 61883-4) instead of DV frames: it arrives
 * here, in chunks of whole 188-byte TS packets, through this handler (set before fwdv_start). */
typedef void (*fwdv_ts_cb)(const uint8_t *ts, size_t len, void *user);
void fwdv_set_ts_handler(fwdv_ts_cb cb, void *user);

int fwdv_open(fwdv_t **out);
void fwdv_close(fwdv_t *d);
/* The device's name, e.g. "Sony DCR-HC21". */
const char *fwdv_name(fwdv_t *d);
int fwdv_start(fwdv_t *d, fwdv_frame_cb cb, void *user);
void fwdv_stop(fwdv_t *d);
/* 1 while the device is still there */
int fwdv_alive(fwdv_t *d);

typedef enum {
    FWDV_PLAY, FWDV_PAUSE, FWDV_STOP, FWDV_REWIND, FWDV_FFWD,
    FWDV_PLAY_REVERSE, FWDV_STEP_BACK, FWDV_STEP_FWD,
    FWDV_RECORD, FWDV_RECORD_PAUSE /* record what is being sent to the camcorder (see fwdv_send_start) */
} fwdv_cmd_t;
int fwdv_transport(fwdv_t *d, fwdv_cmd_t cmd);

typedef enum {
    FWDV_MODE_UNKNOWN = 0, FWDV_MODE_STOPPED, FWDV_MODE_PLAYING, FWDV_MODE_PAUSED,
    FWDV_MODE_REWINDING, FWDV_MODE_FFWD, FWDV_MODE_REVERSE, FWDV_MODE_RECORDING,
    FWDV_MODE_CAMERA /* camera mode: a live picture, no tape */
} fwdv_mode_t;

typedef struct {
    fwdv_mode_t mode;
    int has_tape;      /* 1, 0, or -1 if the device does not say */
    int tc_valid;
    int tc_h, tc_m, tc_s, tc_f;
} fwdv_status_t;
int fwdv_status(fwdv_t *d, fwdv_status_t *st);

/* ---- sending DV to the camcorder (recording onto its tape) ----
 * fwdv_send_start opens an isochronous transmit channel to the device's input plug (stop
 * receiving first: fwdv_stop). fwdv_send_frame then queues whole DV frames (120000 or 144000
 * bytes) and blocks while the queue is full, so the caller is paced at the real frame rate once
 * it is ahead; *abort (may be NULL) ends the wait. The tape must be told to FWDV_RECORD after the
 * stream is running. fwdv_send_stop(drain=1) lets what is queued play out first.
 * macOS only for now; the others return FWDV_ERR_UNSUPPORTED. */
int fwdv_send_start(fwdv_t *d, int pal);
int fwdv_send_frame(fwdv_t *d, const uint8_t *frame, size_t len, volatile int *abort);
void fwdv_send_stop(fwdv_t *d, int drain);
/* frames accepted and frames actually put on the bus so far */
void fwdv_send_stats(fwdv_t *d, uint64_t *queued, uint64_t *sent);

void fwdv_set_verbose(int level);

/* ---- shared by the backends that receive CIP packets themselves (fwdv_dv.c) ---- */

typedef struct {
    uint8_t buf[DV_FRAME_625];
    int len, started;
    uint64_t frames, dropped;
    fwdv_frame_cb cb;
    void *user;
    uint8_t ts[188 * 40]; /* HDV: TS packets waiting to be handed over */
    int ts_len;
} dv_assembler_t;

/* p: an isochronous packet's payload starting at its CIP header, n bytes */
void dv_assemble(dv_assembler_t *a, const uint8_t *p, size_t n);

/* AV/C helpers on raw frames: the frames come back from the device as built
 * here, and the answer is parsed into a status; ctype is the response byte. */
int avc_cmd_bytes(fwdv_cmd_t cmd, uint8_t out[8]);
void avc_parse_transport(const uint8_t *r, int n, fwdv_status_t *st);
void avc_parse_timecode(const uint8_t *r, int n, fwdv_status_t *st);
void avc_parse_medium(const uint8_t *r, int n, fwdv_status_t *st);

/* ---- DV -> isochronous packets (IEC 61883-2), one call per bus cycle (125 us) ---- */

#define DV_TX_AHEAD 4 /* cycles between sending a packet and the time stamp in it */
typedef struct {
    int pal, idx, npkt, no_syt; /* no_syt: send SYT = 0xFFFF (no time stamps) */
    uint8_t dbc, sid;           /* sid: our node number, the packets' source */
    uint64_t cycle;       /* the bus cycle the next packet goes out in (counted from the start) */
    double next_pt, step; /* presentation time of the next data packet, in 24.576 MHz ticks */
    const uint8_t *frame; /* the frame being sent, or NULL (then the packets are empty) */
    int frame_done;       /* set when the last packet of a frame was produced */
} dv_tx_t;

void dv_tx_init(dv_tx_t *t, int pal, uint32_t start_cycle);
/* Writes the next cycle's packet to out (at least 488 bytes): 488 bytes carrying 6 DIF blocks of
 * the frame, or 8 bytes (an empty packet) when it is not time or there is no frame. Returns the size. */
int dv_tx_packet(dv_tx_t *t, uint8_t *out);

extern int fwdv_verbose;

#endif
