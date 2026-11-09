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
    FWDV_PLAY_REVERSE, FWDV_STEP_BACK, FWDV_STEP_FWD
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

void fwdv_set_verbose(int level);

/* ---- shared by the backends that receive CIP packets themselves (fwdv_dv.c) ---- */

typedef struct {
    uint8_t buf[DV_FRAME_625];
    int len, started;
    uint64_t frames, dropped;
    fwdv_frame_cb cb;
    void *user;
} dv_assembler_t;

/* p: an isochronous packet's payload starting at its CIP header, n bytes */
void dv_assemble(dv_assembler_t *a, const uint8_t *p, size_t n);

/* AV/C helpers on raw frames: the frames come back from the device as built
 * here, and the answer is parsed into a status; ctype is the response byte. */
int avc_cmd_bytes(fwdv_cmd_t cmd, uint8_t out[8]);
void avc_parse_transport(const uint8_t *r, int n, fwdv_status_t *st);
void avc_parse_timecode(const uint8_t *r, int n, fwdv_status_t *st);
void avc_parse_medium(const uint8_t *r, int n, fwdv_status_t *st);

extern int fwdv_verbose;

#endif
