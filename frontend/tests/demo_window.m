/* Demo: shows the window with a colour-cycling CAMetalLayer, the overlay (F1), the swkbd bar (T) and prints controller
   state in the title.  Quits after 20 s or with Cmd+Q.  Screenshot: `screencapture -x out.png` while it runs. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "napple_frontend.h"
#include "napple_input.h"

static napple_frontend *g_fe;
static void render(void *u) {
    (void)u;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    CAMetalLayer *layer = (__bridge CAMetalLayer *)napple_frontend_metal_layer(g_fe);
    layer.device = dev;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        napple_frame_limiter lim;
        napple_settings s;
        double t = 0;
        for (;;) {
            napple_frontend_get_settings(g_fe, &s);
            napple_frame_limiter_init(&lim, s.fps_cap);
            @autoreleasepool {
                id<CAMetalDrawable> d = [layer nextDrawable];
                if (!d) {
                    usleep(5000);
                    continue;
                }
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = d.texture;
                p.colorAttachments[0].loadAction = MTLLoadActionClear;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                t += 0.01;
                p.colorAttachments[0].clearColor = MTLClearColorMake(0.5 + 0.5 * sin(t), 0.5 + 0.5 * sin(t * 1.3 + 2), 0.5 + 0.5 * sin(t * 0.7 + 4), 1);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
                [cb presentDrawable:d];
                [cb commit];
                napple_frontend_frame_presented(g_fe);
            }
            napple_frame_limiter_wait(&lim);
        }
    });
    if (getenv("NAPPLE_DEMO_OVERLAY"))
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ napple_frontend_show_overlay(g_fe, 1); });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ napple_frontend_request_quit(g_fe); });
}
int main(void) {
    @autoreleasepool {
        napple_input *in;
        napple_input_create(&(napple_input_config){.title_id = "0000000000000001", .use_game_controller = 1}, &in);
        napple_input_start(in);
        napple_frontend_config c = {.title_id = "0000000000000001", .title_name = "Napple Demo", .input = in, .callbacks = {.on_ready = render}};
        g_fe = napple_frontend_create(&c);
        napple_manifest_dlc dlc[1] = {{"0000000000000A01", "Demo DLC"}};
        napple_manifest_update up[2] = {{"1.2.0", "Update 1.2.0"}, {"1.1.0", ""}};
        napple_manifest m = {
            .title_id = "0000000000000001", .title_name = "Napple Demo", .base_version = "1.0.0", .dlc = dlc, .dlc_count = 1, .updates = up, .update_count = 2};
        napple_frontend_set_manifest(g_fe, &m);
        napple_frontend_run(g_fe);
        napple_input_destroy(in);
    }
    return 0;
}
