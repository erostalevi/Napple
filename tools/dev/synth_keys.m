/* synth_keys: posts real keyboard events (CGEvent) to a process, for end-to-end input tests.
   usage: synth_keys <pid> <delay-secs> <mac-keycode>[:hold-secs] ...    (prefix with + for a chord start, see below)
   Chords: "12+14:0.3" presses keycodes 12 and 14 together. Needs Accessibility trust for the terminal app (checked and reported). */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: synth_keys <pid|0=frontmost> <delay> <key[+key][:hold]> ...\n");
            return 2;
        }
        pid_t pid = (pid_t)atoi(argv[1]);
        double delay = atof(argv[2]);
        fprintf(stderr, "AXIsProcessTrusted=%d\n", (int)AXIsProcessTrusted());
        [NSThread sleepForTimeInterval:delay];
        for (int i = 3; i < argc; i++) {
            char *spec = strdup(argv[i]);
            double hold = 0.15;
            char *c = strchr(spec, ':');
            if (c) {
                *c = 0;
                hold = atof(c + 1);
            }
            int keys[8], n = 0;
            for (char *t = strtok(spec, "+"); t && n < 8; t = strtok(NULL, "+")) keys[n++] = atoi(t);
            for (int d = 1; d >= 0; d--) {
                for (int k = 0; k < n; k++) {
                    CGEventRef e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)keys[k], d);
                    if (pid > 0) CGEventPostToPid(pid, e);
                    else CGEventPost(kCGHIDEventTap, e);
                    CFRelease(e);
                }
                if (d) [NSThread sleepForTimeInterval:hold];
            }
            [NSThread sleepForTimeInterval:0.3];
            free(spec);
        }
    }
    return 0;
}
