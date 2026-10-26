/* Demo: prints the merged Switch controller state (keyboard is not available here: connect a pad). 5 seconds. */
#import <Foundation/Foundation.h>
#include "napple_input.h"
int main(void) {
    @autoreleasepool {
        napple_input *in;
        napple_input_config c = {.title_id = "0000000000000000", .use_game_controller = 1};
        if (napple_input_create(&c, &in)) return 1;
        napple_input_start(in);
        for (int i = 0; i < 25; i++) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
            napple_input_snapshot s;
            napple_input_get_snapshot(in, &s);
            printf("controllers=%d (%s) buttons=%07llx L=(%d,%d) R=(%d,%d)\n", s.controllers, s.controller_name, (unsigned long long)s.pad.buttons,
                   s.pad.left.x, s.pad.left.y, s.pad.right.x, s.pad.right.y);
        }
        napple_input_destroy(in);
    }
    return 0;
}
