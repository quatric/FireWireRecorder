/*
 * Linux monitoring output through ALSA's "default" device (the sound server
 * on desktop systems). libasound is loaded at run time, so building needs no
 * ALSA headers and the app still runs, silently, where it is missing.
 */
#include "sysaudio.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct snd_pcm snd_pcm_t;
enum { PCM_PLAYBACK = 0, FMT_S16_LE = 2, ACCESS_RW_INTERLEAVED = 3 };

static struct {
    int loaded;
    int (*open)(snd_pcm_t **, const char *, int, int);
    int (*set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
    long (*writei)(snd_pcm_t *, const void *, unsigned long);
    int (*recover)(snd_pcm_t *, int, int);
    int (*close)(snd_pcm_t *);
} A;

static void alsa_dlopen(void)
{
    void *h = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return;
    *(void **)&A.open = dlsym(h, "snd_pcm_open");
    *(void **)&A.set_params = dlsym(h, "snd_pcm_set_params");
    *(void **)&A.writei = dlsym(h, "snd_pcm_writei");
    *(void **)&A.recover = dlsym(h, "snd_pcm_recover");
    *(void **)&A.close = dlsym(h, "snd_pcm_close");
    A.loaded = A.open && A.set_params && A.writei && A.recover && A.close;
}

static int alsa_load(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, alsa_dlopen);
    return A.loaded;
}

struct sa_output {
    snd_pcm_t *pcm;
    sa_output_cb cb;
    void *user;
    volatile int quit;
    pthread_t thread;
};

static void *playback_thread(void *arg)
{
    sa_output_t *out = arg;
    int16_t buf[480 * 2];
    while (!out->quit) {
        out->cb(buf, 480, out->user);
        long n = A.writei(out->pcm, buf, 480);
        if (n < 0 && A.recover(out->pcm, (int)n, 1) < 0)
            break;
    }
    return NULL;
}

sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user)
{
    if (!alsa_load())
        return NULL;
    sa_output_t *out = calloc(1, sizeof(*out));
    if (!out)
        return NULL;
    if (A.open(&out->pcm, "default", PCM_PLAYBACK, 0) < 0) {
        free(out);
        return NULL;
    }
    if (A.set_params(out->pcm, FMT_S16_LE, ACCESS_RW_INTERLEAVED, 2, (unsigned)rate, 1, 60000) < 0 ||
        (out->cb = cb, out->user = user, pthread_create(&out->thread, NULL, playback_thread, out))) {
        A.close(out->pcm);
        free(out);
        return NULL;
    }
    return out;
}

void sa_output_stop(sa_output_t *out)
{
    if (!out)
        return;
    out->quit = 1;
    pthread_join(out->thread, NULL);
    A.close(out->pcm);
    free(out);
}
