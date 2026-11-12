/*
 * FireWireRecorder engine shared by the macOS and Windows apps: finds a DV
 * camcorder or deck on FireWire, receives its DV stream, records it (as raw
 * DV frame for frame, or encoded to MPEG-2 or H.264 by ffmpeg), decodes it
 * with ffmpeg for the preview picture, the level meters and audio monitoring,
 * and drives the tape. Everything here is thread safe; the GUIs poll it from
 * a timer.
 */
#ifndef FWDV_APP_CORE_H
#define FWDV_APP_CORE_H

#include <stdint.h>
#include <stdio.h>

#include "fwdv.h"

#define APP_NAME "FireWireRecorder"

typedef struct app app_t;

typedef enum {
    APP_SEARCHING, /* no DV device yet; retried every couple of seconds */
    APP_BUSY,      /* found, but another program is using it */
    APP_NO_SIGNAL, /* connected, nothing coming in (tape stopped, or no tape) */
    APP_STREAMING,
} app_state_t;

/* Recording formats, in the order the GUIs list them. */
enum { APP_FMT_DV = 0, APP_FMT_MPEG2, APP_FMT_H264, APP_FORMATS };
extern const char *const app_format_names[APP_FORMATS];
extern const char *const app_format_ext[APP_FORMATS];

/* Starts the engine; it connects to the device in the background. */
app_t *app_start(void);
void app_close(app_t *a); /* finishes a recording in progress */
void app_retry(app_t *a); /* look for the device again now */

app_state_t app_state(app_t *a);
/* One line for the status bar: what the engine is doing, or the last problem. */
void app_status(app_t *a, char *out, int len);
/* The device's name, or "" when there is none. */
void app_device_name(app_t *a, char *out, int len);
/* The incoming picture: 720x480 at 30 (29.97) or 720x576 at 25; 0 when there is none. */
int app_signal(app_t *a, int *w, int *h, int *fps);

/* ---- tape ---- */
int app_tape(app_t *a, fwdv_cmd_t cmd); /* 0, or an FWDV_ERR_* */
/* The last status read from the device (every half second); 0 if there is one. */
int app_tape_status(app_t *a, fwdv_status_t *st);
/* When the picture coming in was shot, from the DV data code the camcorder recorded with it:
 * "2003-07-14 15:42:07" (or just the date). 1 if there is one; 0 (out = "") with no DV picture,
 * or when the tape carries no date. */
int app_tape_datetime(app_t *a, char *out, int len);

/* ---- preview ----
 * Frames are 32-bit pixels 0x00RRGGBB (BGRX bytes, a Windows DIB), 720x480 or
 * 720x576. app_get_frame copies the latest frame into buf if it is w x h and
 * newer than *seq, and returns 1 then. With the preview off, nothing is
 * decoded for the picture at all (the sound and meters keep working). */
void app_set_preview(app_t *a, int on);
int app_preview(app_t *a);
void app_frame_size(app_t *a, int *w, int *h);
int app_get_frame(app_t *a, uint32_t *buf, int w, int h, uint64_t *seq);
/* 4:3, or 16:9 when the camcorder flagged the picture widescreen */
double app_aspect(app_t *a);

/* ---- sound: peak levels since the last call in dBFS (-60..0), monitoring ---- */
void app_peaks(app_t *a, double db[2]);
void app_set_monitor(app_t *a, int on);
/* Fills frames of 48 kHz stereo for the speakers (macOS calls this from its
 * own audio output; Windows and the old Macs play it themselves). 0 if silent. */
int app_audio_render(app_t *a, int16_t *out, int frames);
int app_audio_render_float(app_t *a, float *left, float *right, int frames);

/* ---- recording ---- */
int app_record_start(app_t *a, const char *folder, int format, char *path, int pathlen, char *err, int errlen);
/* Stops in the background; done(user, ok, path) runs on a worker thread. */
typedef void (*app_done_cb)(void *user, int ok, const char *path);
void app_record_stop(app_t *a, app_done_cb done, void *user);
int app_is_recording(app_t *a);
double app_record_seconds(app_t *a); /* of the tape recorded, by frames */
uint64_t app_record_bytes(app_t *a); /* written to the file so far */
/* "Stop at end of tape": with it on, app_auto_stop_due() turns 1 once the
 * tape stops or the picture ends during a recording; the GUI then stops it. */
void app_set_auto_stop(app_t *a, int on);
int app_auto_stop_due(app_t *a);
/* "Stop after": the choices (minutes; 0 = no limit) and their names, shared by every GUI. The GUI polls
 * app_stop_after_due() and stops the recording itself when it returns 1. */
#define APP_STOP_AFTERS 14
extern const int app_stop_after_minutes[APP_STOP_AFTERS];
extern const char *const app_stop_after_names[APP_STOP_AFTERS];
void app_set_stop_after(app_t *a, int index);
int app_stop_after_due(app_t *a);
/* A custom time: any number of minutes (0 = no limit), up to APP_STOP_AFTER_MAX. */
#define APP_STOP_AFTER_MAX 5999
void app_set_stop_after_minutes(app_t *a, int minutes);
/* Reads what a person typed: "45", "45 min", "90m", "2h", "1.5 hours", "1:30" or "no limit". Returns the
 * minutes (0 = no limit) or -1 if the text is not a time. */
int app_parse_stop_after(const char *text);
/* "No limit", "5 min", "1 hour", "2 h 15 min"... for any number of minutes. */
void app_stop_after_label(int minutes, char *buf, int buflen);
/* The saved choice: the minutes (0 = no limit). `old_index` is what older versions stored (a preset's
 * position), or -1. */
int app_stop_after_from_index(int index);

/* ---- recording a video file onto the camcorder's tape (macOS) ----
 * Any file ffmpeg can read is converted to DV (letterboxed to 4:3, in the standard of the tape last
 * seen: NTSC unless the camcorder played PAL) and sent to the camcorder, which records it from the
 * tape's current position. It runs in the background: poll app_send_status. */
int app_send_supported(void);
int app_send_start(app_t *a, const char *file, char *err, int errlen);
void app_send_cancel(app_t *a);
int app_send_peek(app_t *a); /* 1 while a recording onto the tape is running; changes nothing */
/* 0 idle, 1 sending (msg: progress), 2 finished, -1 failed (msg says why). 2 and -1 are reported once. */
int app_send_status(app_t *a, double *seconds, char *msg, int len);

/* Saves the current picture as a square-pixel PNG (needs the preview on). */
int app_snapshot(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen);

/* "FireWireRecorder 2026-11-09 at 21.05.33.ext" in folder. */
void app_timestamped_path(const char *folder, const char *ext, char *out, int len);
/* Movies (macOS) or Videos (Windows, Linux), or the home folder. */
void app_default_folder(char *out, int len);
/* fopen with a UTF-8 path, also on Windows */
FILE *app_fopen(const char *path, const char *mode);

/* settings for the Windows app (the Mac app uses NSUserDefaults) */
int app_setting_int(const char *key, int def);
void app_setting_str(const char *key, char *out, int len, const char *def);
void app_set_setting_int(const char *key, int v);
void app_set_setting_str(const char *key, const char *v);

#endif
