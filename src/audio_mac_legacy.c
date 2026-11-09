/*
 * Monitor playback for the Mac OS X 10.3/10.4 build (PowerPC and Intel),
 * which has no AVAudioEngine: the default output unit, found with the
 * Component Manager, converts our 16-bit stereo itself.
 */
#include "sysaudio.h"

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>
#include <stdlib.h>
#include <string.h>

/* ---- output ---- */

struct sa_output {
    AudioUnit unit;
    sa_output_cb cb;
    void *user;
};

static OSStatus render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *abl)
{
    (void)flags, (void)ts, (void)bus;
    sa_output_t *o = ctx;
    o->cb((int16_t *)abl->mBuffers[0].mData, (int)frames, o->user);
    return noErr;
}

sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user)
{
    ComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple,
                               0, 0 };
    Component comp = FindNextComponent(NULL, &d);
    if (!comp)
        return NULL;
    sa_output_t *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->cb = cb;
    o->user = user;
    if (OpenAComponent(comp, &o->unit)) {
        free(o);
        return NULL;
    }
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof(f));
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked
#if defined(__BIG_ENDIAN__)
                     | kLinearPCMFormatFlagIsBigEndian
#endif
        ;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    AURenderCallbackStruct rcb = { render, o };
    if (AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) ||
        AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rcb,
                             sizeof(rcb)) ||
        AudioUnitInitialize(o->unit) || AudioOutputUnitStart(o->unit)) {
        CloseComponent(o->unit);
        free(o);
        return NULL;
    }
    return o;
}

void sa_output_stop(sa_output_t *o)
{
    if (!o)
        return;
    AudioOutputUnitStop(o->unit);
    AudioUnitUninitialize(o->unit);
    CloseComponent(o->unit);
    free(o);
}
