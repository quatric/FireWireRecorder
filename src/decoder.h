/*
 * An ffmpeg child process that reads the DV stream on stdin
 * and writes decoded data (raw video frames or PCM) to stdout. The app runs
 * one for the preview picture and one for the sound.
 */
#ifndef FWDV_DECODER_H
#define FWDV_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct decoder decoder_t;
/* Called on the decoder's reader thread with `chunk` bytes at a time
 * (or whatever arrived, if chunk is 0). */
typedef void (*dec_out_cb)(const uint8_t *data, size_t len, void *user);

/* args: ffmpeg's arguments after the program name, NULL-terminated. */
decoder_t *dec_start(const char *const *args, size_t chunk, dec_out_cb cb, void *user, char *err, int errlen);
/* Same with an input queue of `queue` bytes; cb may be NULL (the output is discarded). For an
 * encoder that writes a file: ffmpeg's own output goes there, not to the pipe. */
decoder_t *dec_start_queue(const char *const *args, size_t chunk, dec_out_cb cb, void *user, size_t queue, char *err,
                           int errlen);
/* Never blocks; drops the data (and returns -1) if ffmpeg falls behind. */
int dec_feed(decoder_t *d, const uint8_t *data, size_t len);
uint64_t dec_dropped(decoder_t *d);
/* Writes what is queued, closes ffmpeg's input and waits for it to exit; 0 if it succeeded. */
int dec_finish(decoder_t *d);
/* Kills ffmpeg without waiting; safe from the output callback (dec_finish or dec_stop still has to follow). */
void dec_abort(decoder_t *d);
/* Ends ffmpeg (killing it if it does not exit) and waits for the reader. */
void dec_stop(decoder_t *d);

/* FWDV_FFMPEG, then next to this program, then PATH. NULL if none. */
const char *dec_find_ffmpeg(void);

#endif
