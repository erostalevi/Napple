/* The library applet sheets (src/applet_sheets.m) in a real AppKit loop: each is answered by NAPPLE_SHEET_AUTO (no human), the guest side runs on a
 * thread of its own like the runtime's. NAPPLE_SHEET_SHOT=<dir> also keeps a PNG of every sheet (the docs screenshots). */
#import <Cocoa/Cocoa.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "napple_frontend.h"

static int g_checked, g_failed;
#define CHECK(c, ...)                                                                                                                                          \
    do {                                                                                                                                                       \
        g_checked++;                                                                                                                                           \
        if (!(c)) {                                                                                                                                            \
            g_failed++;                                                                                                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                                                               \
            fprintf(stderr, __VA_ARGS__);                                                                                                                      \
            fprintf(stderr, "\n");                                                                                                                             \
        }                                                                                                                                                      \
    } while (0)

static void guest(void) {
    int players = 2;
    setenv("NAPPLE_SHEET_AUTO", "confirm:300", 1);
    CHECK(napple_frontend_sheet_controller(NULL, 1, 4, 2, 0, &players) == 1 && players == 2, "controller confirm keeps %d", players);
    players = 9;
    CHECK(napple_frontend_sheet_controller(NULL, 1, 4, 1, 0, &players) == 1 && players == 4, "controller: out-of-range default is clamped (%d)", players);
    setenv("NAPPLE_SHEET_AUTO", "cancel:300", 1);
    int rr = napple_frontend_sheet_controller(NULL, 2, 2, 1, 1, &players); CHECK(rr == 0, "controller cancel %d", rr);

    uint8_t uids[3][16];
    memset(uids, 0x4e, sizeof uids);
    uids[1][0] = 0x01;
    setenv("NAPPLE_SHEET_AUTO", "confirm:300", 1);
    CHECK(napple_frontend_sheet_player_select(NULL, (const uint8_t(*)[16])uids, 3) == 0, "player select confirm: the first profile");
    setenv("NAPPLE_SHEET_AUTO", "cancel:300", 1);
    CHECK(napple_frontend_sheet_player_select(NULL, (const uint8_t(*)[16])uids, 3) == -1, "player select cancel");

    setenv("NAPPLE_SHEET_AUTO", "confirm:300", 1);
    napple_frontend_sheet_error(NULL, "The game reported an error", "Error code 2002-0002\nThe network service is not available.");
    CHECK(1, "error sheet returns once dismissed");

    uint8_t px[64 * 48 * 4];
    for (unsigned i = 0; i < 64 * 48; i++) {
        px[i * 4] = (uint8_t)(i % 64 * 4);
        px[i * 4 + 1] = (uint8_t)(i / 64 * 5);
        px[i * 4 + 2] = 160;
        px[i * 4 + 3] = 255;
    }
    setenv("NAPPLE_SHEET_AUTO", "confirm:300", 1);
    CHECK(napple_frontend_sheet_image(NULL, px, 64, 48, 0, 3, 1) == 1, "album: select");
    setenv("NAPPLE_SHEET_AUTO", "next:300", 1);
    CHECK(napple_frontend_sheet_image(NULL, px, 64, 48, 0, 3, 1) == 0, "album: next");
    CHECK(napple_frontend_sheet_image(NULL, px, 64, 48, 2, 3, 0) == -1, "album: the last image's Next is Done (leave)");
    setenv("NAPPLE_SHEET_AUTO", "leave:300", 1);
    CHECK(napple_frontend_sheet_image(NULL, px, 64, 48, 1, 3, 1) == -1, "album: close");

    char last[256] = "";
    setenv("NAPPLE_SHEET_AUTO", "confirm:1800", 1);
    const char *dir = getenv("TMPDIR") ?: "/tmp";
    char path[512], url[600];
    snprintf(path, sizeof path, "%s/napple_sheet_page.html", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("<html><body style='font:28px sans-serif;padding:40px;background:#fff;color:#111'><h1>Offline manual</h1><p>Napple web applet</p></body></html>", f);
        fclose(f);
    }
    snprintf(url, sizeof url, "file://%s", path);
    int r = napple_frontend_sheet_web(NULL, url, dir, last, sizeof last);
    CHECK(r == 0 && strstr(last, "napple_sheet_page.html"), "web: Done returns 0 with the page url (%d, %s)", r, last);
    r = napple_frontend_sheet_web(NULL, "ftp://example.com/", NULL, last, sizeof last);
    CHECK(r == -1, "web: an unsupported scheme cannot be shown (%d)", r);
    unlink(path);
    printf("{\"test\":\"sheets\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    exit(g_failed != 0);
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        /* the guest starts once the app finished launching: a modal loop opened earlier returns at once */
        [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidFinishLaunchingNotification
                                                          object:nil
                                                           queue:nil
                                                      usingBlock:^(NSNotification *n) {
                                                          (void)n;
                                                          dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ guest(); });
                                                      }];
        [NSApp run];
    }
    return 0;
}
