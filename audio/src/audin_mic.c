/* audin microphone capture (macOS): an AUHAL input unit on the default input device feeding the mic ring of audin_source.c with 48 kHz mono
   float. Started only when NAPPLE_AUDIN_SOURCE=mic: the process needs NSMicrophoneUsageDescription and the audio-input entitlement
   (packaging) before this is on by default. Not built or run on Linux; the Linux suite links the unsupported stub below. */
#include "internal.h"
#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_OSX
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdlib.h>

static struct {
    AudioComponentInstance unit;
    int running;
    float *scratch;
    UInt32 scratch_frames;
} U;

static OSStatus input_cb(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *unused) {
    (void)ctx;
    (void)unused;
    if (frames > U.scratch_frames) return noErr;
    AudioBufferList abl = {1, {{1, frames * (UInt32)sizeof(float), U.scratch}}};
    if (AudioUnitRender(U.unit, flags, ts, bus, frames, &abl) == noErr) nae_mic_ring_push(U.scratch, frames);
    return noErr;
}

int nae_mic_start(void) {
    if (U.running) return 0;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &U.unit)) return -1;
    UInt32 on = 1, off = 0;
    AudioDeviceID dev = 0;
    UInt32 sz = sizeof dev;
    AudioObjectPropertyAddress def = {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioStreamBasicDescription f = {48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 4, 1, 4, 1, 32, 0};
    AURenderCallbackStruct cb = {input_cb, NULL};
    U.scratch_frames = 4096;
    U.scratch = calloc(U.scratch_frames, sizeof(float));
    if (!U.scratch || AudioObjectGetPropertyData(kAudioObjectSystemObject, &def, 0, NULL, &sz, &dev) || !dev ||
        AudioUnitSetProperty(U.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on) ||
        AudioUnitSetProperty(U.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off) ||
        AudioUnitSetProperty(U.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev) ||
        AudioUnitSetProperty(U.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f) ||
        AudioUnitSetProperty(U.unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb) || AudioUnitInitialize(U.unit) ||
        AudioOutputUnitStart(U.unit)) {
        napple_audio_log(NULL, "microphone capture could not start (device, permission or format)");
        AudioComponentInstanceDispose(U.unit);
        free(U.scratch);
        U.scratch = NULL;
        return -1;
    }
    U.running = 1;
    return 0;
}

void nae_mic_stop(void) {
    if (!U.running) return;
    AudioOutputUnitStop(U.unit);
    AudioUnitUninitialize(U.unit);
    AudioComponentInstanceDispose(U.unit);
    free(U.scratch);
    U.scratch = NULL;
    U.running = 0;
}
#else
int nae_mic_start(void) { return -1; }
void nae_mic_stop(void) {}
#endif
