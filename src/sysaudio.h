/*
 * Playback to the default output device, for monitoring the camcorder's sound.
 *
 *   Windows  waveOut (audio_windows.c)
 *   Linux    ALSA, loaded at run time with dlopen (audio_alsa.c)
 *   macOS    the app plays through AVAudioEngine itself (viewer.m); the build for
 *            Mac OS X 10.3-10.8 uses the default output unit (audio_mac_legacy.c)
 */
#ifndef FWDV_SYSAUDIO_H
#define FWDV_SYSAUDIO_H

#include <stdint.h>

/* cb fills `frames` interleaved 16-bit stereo frames from the audio thread. */
typedef struct sa_output sa_output_t;
typedef void (*sa_output_cb)(int16_t *out, int frames, void *user);
sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user);
void sa_output_stop(sa_output_t *out);

#endif
