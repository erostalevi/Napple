/* Extra Core Audio declarations for `clang -fsyntax-only` of audio/src/audin_mic.c on Linux (see tools/linux-check/README.md). Declarations only. */
#include <AudioToolbox/AudioToolbox.h>
enum {
    kAudioUnitSubType_HALOutput = 100,
    kAudioOutputUnitProperty_EnableIO = 101,
    kAudioOutputUnitProperty_CurrentDevice = 102,
    kAudioOutputUnitProperty_SetInputCallback = 103,
    kAudioUnitScope_Output = 104,
    kAudioUnitScope_Global = 105,
    kAudioHardwarePropertyDefaultInputDevice = 106
};
OSStatus AudioUnitRender(AudioComponentInstance, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);
