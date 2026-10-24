/* Linux stand-in for <CoreAudio/CoreAudio.h>; see AudioToolbox/AudioToolbox.h. */
#ifndef NAPPLE_LINUX_COREAUDIO_H
#define NAPPLE_LINUX_COREAUDIO_H
#include <AudioToolbox/AudioToolbox.h>
static inline OSStatus AudioObjectGetPropertyData(uint32_t o, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 *n, void *d) { (void)o; (void)a; (void)q; (void)qd; (void)n; (void)d; return -1; }
static inline OSStatus AudioObjectSetPropertyData(uint32_t o, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 n, const void *d) { (void)o; (void)a; (void)q; (void)qd; (void)n; (void)d; return -1; }
#endif
