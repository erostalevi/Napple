/* audin microphone capture (macOS): an AUHAL input unit on the default input device feeding the mic ring of audin_source.c with 48 kHz mono
   float. On by default when the app may use it (nae_mic_permission below; generated apps carry NSMicrophoneUsageDescription and the audio-input
   entitlement), or when NAPPLE_AUDIN_SOURCE=mic asks for it; denied or absent permission leaves the guest recording silence.
   Not built or run on Linux; the Linux suite links the unsupported stub below. */
#include "internal.h"
#include <TargetConditionals.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && TARGET_OS_OSX
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdlib.h>
#include <string.h>

/* AVCaptureDevice authorizationStatusForMediaType:@"soun" through the ObjC runtime (this file stays plain C and links no AVFoundation).
   0 notDetermined, 1 restricted, 2 denied, 3 authorized. The env NAPPLE_AUDIN_MIC_PERMISSION=granted|undetermined|denied overrides it (tests, CI). */
int nae_mic_permission(void) {
    const char *ov = getenv("NAPPLE_AUDIN_MIC_PERMISSION");
    if (ov && *ov) return !strcmp(ov, "granted") ? NAE_MIC_GRANTED : !strcmp(ov, "undetermined") ? NAE_MIC_UNDETERMINED : NAE_MIC_DENIED;
    if (!dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_LAZY)) return NAE_MIC_DENIED;
    Class cls = objc_getClass("AVCaptureDevice");
    if (!cls) return NAE_MIC_DENIED;
    long st = ((long (*)(Class, SEL, CFStringRef))objc_msgSend)(cls, sel_registerName("authorizationStatusForMediaType:"), CFSTR("soun"));
    return st == 3 ? NAE_MIC_GRANTED : st == 0 ? NAE_MIC_UNDETERMINED : NAE_MIC_DENIED;
}
/* A process that has no NSMicrophoneUsageDescription is killed by the system when it first touches the microphone: only an app bundle that
   carries one may trigger the prompt (a command-line run is "undetermined" and stays silent). */
int nae_mic_can_prompt(void) {
    CFBundleRef b = CFBundleGetMainBundle();
    return b && CFBundleGetValueForInfoDictionaryKey(b, CFSTR("NSMicrophoneUsageDescription")) != NULL;
}

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
int nae_mic_permission(void) {
    const char *ov = getenv("NAPPLE_AUDIN_MIC_PERMISSION");
    return ov && !strcmp(ov, "granted") ? NAE_MIC_GRANTED : NAE_MIC_DENIED; /* nothing can capture here: the start below fails anyway */
}
int nae_mic_can_prompt(void) { return 0; }
int nae_mic_start(void) { return -1; }
void nae_mic_stop(void) {}
#endif
