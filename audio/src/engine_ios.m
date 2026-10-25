/* iOS audio session for the output engine (engine.c): playback category, small buffer, interruption and route-change recovery.
 * Compiled only by the iOS build (Makefile.ios); the macOS engine talks to the HAL directly. */
#import <AVFoundation/AVFoundation.h>
#include "internal.h"

void napple_audio_output_restart(int recreate);

unsigned nae_ios_session_start(double sample_rate, double buffer_seconds) {
    static dispatch_once_t once;
    AVAudioSession *s = AVAudioSession.sharedInstance;
    NSError *err = nil;
    /* Playback: sound while the silent switch is on (a game), mixes with nothing else. */
    if (![s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err]) napple_audio_log(NULL, "AVAudioSession category: %s", err.localizedDescription.UTF8String);
    [s setPreferredSampleRate:sample_rate error:nil];
    [s setPreferredIOBufferDuration:buffer_seconds error:nil];
    if (![s setActive:YES error:&err]) napple_audio_log(NULL, "AVAudioSession activate: %s", err.localizedDescription.UTF8String);
    dispatch_once(&once, ^{
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            if ([n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeEnded) {
                [AVAudioSession.sharedInstance setActive:YES error:nil];
                napple_audio_output_restart(0);                       /* a call or Siri stopped the unit: start it again */
            }
        }];
        [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; napple_audio_output_restart(1); }];
    });
    unsigned frames = (unsigned)(s.IOBufferDuration * s.sampleRate + 0.5);
    napple_audio_log(NULL, "AVAudioSession: %.0f Hz, I/O buffer %u frames", s.sampleRate, frames);
    return frames ? frames : 512;
}
