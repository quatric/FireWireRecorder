/* Windows monitoring output through waveOut, which runs on every Windows from 7 on. */
#include "sysaudio.h"

#include <windows.h>
#include <mmsystem.h>
#include <pthread.h>
#include <stdlib.h>

#define OUT_BUFS 6
#define OUT_FRAMES 480 /* 10 ms at 48 kHz */

struct sa_output {
    HWAVEOUT wo;
    HANDLE event;
    WAVEHDR hdr[OUT_BUFS];
    int16_t buf[OUT_BUFS][OUT_FRAMES * 2];
    int frames;
    sa_output_cb cb;
    void *user;
    volatile int quit;
    int thread_started;
    pthread_t thread;
};

static void *playback_thread(void *arg)
{
    sa_output_t *out = arg;
    while (!out->quit) {
        for (int i = 0; i < OUT_BUFS; i++) {
            WAVEHDR *h = &out->hdr[i];
            if (h->dwUser && !(h->dwFlags & WHDR_DONE))
                continue; /* still queued */
            out->cb(out->buf[i], out->frames, out->user);
            h->dwUser = 1;
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(out->wo, h, sizeof(*h));
        }
        WaitForSingleObject(out->event, 50);
    }
    return NULL;
}

sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user)
{
    sa_output_t *out = calloc(1, sizeof(*out));
    if (!out)
        return NULL;
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, (DWORD)rate, (DWORD)rate * 4, 4, 16, 0 };
    out->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!out->event ||
        waveOutOpen(&out->wo, WAVE_MAPPER, &wf, (DWORD_PTR)out->event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        if (out->event)
            CloseHandle(out->event);
        free(out);
        return NULL;
    }
    out->frames = rate / 100 < OUT_FRAMES ? rate / 100 : OUT_FRAMES;
    for (int i = 0; i < OUT_BUFS; i++) {
        out->hdr[i].lpData = (LPSTR)out->buf[i];
        out->hdr[i].dwBufferLength = (DWORD)out->frames * 4;
        waveOutPrepareHeader(out->wo, &out->hdr[i], sizeof(WAVEHDR));
    }
    out->cb = cb;
    out->user = user;
    if (pthread_create(&out->thread, NULL, playback_thread, out)) {
        sa_output_stop(out);
        return NULL;
    }
    out->thread_started = 1;
    return out;
}

void sa_output_stop(sa_output_t *out)
{
    if (!out)
        return;
    if (out->thread_started) {
        out->quit = 1;
        SetEvent(out->event);
        pthread_join(out->thread, NULL);
    }
    waveOutReset(out->wo);
    for (int i = 0; i < OUT_BUFS; i++)
        waveOutUnprepareHeader(out->wo, &out->hdr[i], sizeof(WAVEHDR));
    waveOutClose(out->wo);
    CloseHandle(out->event);
    free(out);
}
