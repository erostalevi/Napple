/* Linux stand-in for <AudioToolbox/AudioToolbox.h>: lets src/engine.c compile and link unchanged so the whole
   device-free test suite runs on Linux.  Every device call fails, which engine.c already treats as "no device"
   (mixing without sound).  The real Core Audio path is only exercised on a Mac. */
#ifndef NAPPLE_LINUX_AUDIOTOOLBOX_H
#define NAPPLE_LINUX_AUDIOTOOLBOX_H
#include <stdint.h>
typedef int32_t OSStatus; typedef uint32_t UInt32; typedef uint32_t AudioDeviceID;
enum { noErr = 0 };
typedef struct OpaqueAudioComponent *AudioComponent;
typedef struct OpaqueAudioComponentInstance *AudioComponentInstance;
typedef struct { uint32_t componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;
typedef struct { double mSampleRate; uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef struct { uint32_t mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { uint32_t mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct { double mSampleTime; } AudioTimeStamp;
typedef uint32_t AudioUnitRenderActionFlags;
typedef OSStatus (*AURenderCallback)(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
typedef struct { double mMinimum, mMaximum; } AudioValueRange;
typedef struct { uint32_t mSelector, mScope, mElement; } AudioObjectPropertyAddress;
enum { kAudioUnitType_Output = 1, kAudioUnitSubType_DefaultOutput = 2, kAudioUnitManufacturer_Apple = 3,
       kAudioFormatLinearPCM = 4, kAudioFormatFlagIsFloat = 1, kAudioFormatFlagIsPacked = 8,
       kAudioUnitProperty_StreamFormat = 8, kAudioUnitProperty_SetRenderCallback = 23, kAudioUnitScope_Input = 1,
       kAudioHardwarePropertyDefaultOutputDevice = 1, kAudioObjectPropertyScopeGlobal = 2, kAudioObjectPropertyElementMain = 0,
       kAudioDevicePropertyBufferFrameSize = 5, kAudioDevicePropertyBufferFrameSizeRange = 6, kAudioObjectSystemObject = 1 };
static inline AudioComponent AudioComponentFindNext(AudioComponent a, const AudioComponentDescription *d) { (void)a; (void)d; return 0; }
static inline OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *i) { (void)c; (void)i; return -1; }
static inline OSStatus AudioComponentInstanceDispose(AudioComponentInstance i) { (void)i; return 0; }
static inline OSStatus AudioUnitSetProperty(AudioComponentInstance i, UInt32 p, UInt32 s, UInt32 e, const void *d, UInt32 n) { (void)i; (void)p; (void)s; (void)e; (void)d; (void)n; return -1; }
static inline OSStatus AudioUnitInitialize(AudioComponentInstance i) { (void)i; return -1; }
static inline OSStatus AudioUnitUninitialize(AudioComponentInstance i) { (void)i; return 0; }
static inline OSStatus AudioOutputUnitStart(AudioComponentInstance i) { (void)i; return -1; }
static inline OSStatus AudioOutputUnitStop(AudioComponentInstance i) { (void)i; return 0; }
#endif
