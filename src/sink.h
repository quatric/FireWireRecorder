/*
 * A byte queue with its own writer thread, so the USB thread never waits on a
 * disk or a pipe. Used for the recording file and for feeding the decoders.
 */
#ifndef FWDV_SINK_H
#define FWDV_SINK_H

#include <stddef.h>
#include <stdint.h>

typedef struct sink sink_t;
/* Writes n bytes; returns 0 on success. Runs on the sink's thread. */
typedef int (*sink_write_fn)(void *ctx, const uint8_t *p, size_t n);

sink_t *sink_start(size_t capacity, sink_write_fn fn, void *ctx);
/* Never blocks. Returns -1 (and counts the bytes as dropped) when full or failed. */
int sink_push(sink_t *s, const uint8_t *p, size_t n);
/* drain: write what is queued first. Returns 0 if every write succeeded. */
int sink_stop(sink_t *s, int drain);
uint64_t sink_written(sink_t *s);
uint64_t sink_dropped(sink_t *s);
int sink_failed(sink_t *s);

#endif
