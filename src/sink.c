#include "sink.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct sink {
    uint8_t *buf;
    size_t cap, head, count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int closing, drain;
    volatile int failed;
    uint64_t written, dropped;
    sink_write_fn fn;
    void *ctx;
    pthread_t thread;
};

static void *writer(void *arg)
{
    sink_t *s = arg;
    uint8_t *chunk = malloc(256 * 1024);
    for (;;) {
        pthread_mutex_lock(&s->lock);
        while (!s->count && !s->closing)
            pthread_cond_wait(&s->cond, &s->lock);
        if (s->closing && (!s->drain || !s->count || s->failed)) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        size_t n = s->count < 256 * 1024 ? s->count : 256 * 1024;
        size_t first = s->cap - s->head < n ? s->cap - s->head : n;
        memcpy(chunk, s->buf + s->head, first);
        memcpy(chunk + first, s->buf, n - first);
        s->head = (s->head + n) % s->cap;
        s->count -= n;
        pthread_mutex_unlock(&s->lock);

        if (!s->failed) {
            if (s->fn(s->ctx, chunk, n))
                s->failed = 1;
            else {
                pthread_mutex_lock(&s->lock);
                s->written += n;
                pthread_mutex_unlock(&s->lock);
            }
        }
    }
    free(chunk);
    return NULL;
}

sink_t *sink_start(size_t capacity, sink_write_fn fn, void *ctx)
{
    sink_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->buf = malloc(capacity);
    if (!s->buf) {
        free(s);
        return NULL;
    }
    s->cap = capacity;
    s->fn = fn;
    s->ctx = ctx;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    if (pthread_create(&s->thread, NULL, writer, s)) {
        free(s->buf);
        free(s);
        return NULL;
    }
    return s;
}

int sink_push(sink_t *s, const uint8_t *p, size_t n)
{
    int r = 0;
    pthread_mutex_lock(&s->lock);
    if (s->closing || s->failed || s->count + n > s->cap) {
        s->dropped += n;
        r = -1;
    } else {
        size_t tail = (s->head + s->count) % s->cap;
        size_t first = s->cap - tail < n ? s->cap - tail : n;
        memcpy(s->buf + tail, p, first);
        memcpy(s->buf, p + first, n - first);
        s->count += n;
        pthread_cond_signal(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
    return r;
}

int sink_stop(sink_t *s, int drain)
{
    if (!s)
        return -1;
    pthread_mutex_lock(&s->lock);
    s->closing = 1;
    s->drain = drain;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread, NULL);
    int r = s->failed ? -1 : 0;
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s->buf);
    free(s);
    return r;
}

uint64_t sink_written(sink_t *s)
{
    pthread_mutex_lock(&s->lock);
    uint64_t v = s->written;
    pthread_mutex_unlock(&s->lock);
    return v;
}

uint64_t sink_dropped(sink_t *s)
{
    pthread_mutex_lock(&s->lock);
    uint64_t v = s->dropped;
    pthread_mutex_unlock(&s->lock);
    return v;
}

int sink_failed(sink_t *s) { return s->failed; }
