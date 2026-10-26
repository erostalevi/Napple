/* Headless-ish window test: creates the frontend without showing the window, exercises settings persistence, overlay,
   manifest, swkbd (scripted answer) and the quit path inside the real AppKit loop. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include <unistd.h>
#include "napple_frontend.h"
#include "napple_input.h"

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

static napple_frontend *g_fe;
static int g_ready, g_changed, g_overlay_on, g_overlay_off, g_quit, g_restart, g_text_ok;
static char g_dir[64];
static char g_text[64];
static void on_ready(void *u) {
    (void)u;
    g_ready++;
}
static void on_changed(void *u, const napple_settings *s) {
    (void)u;
    (void)s;
    g_changed++;
}
static void on_overlay(void *u, int v) {
    (void)u;
    if (v) g_overlay_on++;
    else g_overlay_off++;
}
static void on_quit(void *u) {
    (void)u;
    g_quit++;
}
static void on_restart(void *u, const napple_settings *s) {
    (void)u;
    g_restart++;
    CHECK(!strcmp(s->update_version, "1.1.0"), "restart carries update choice");
}
static void text_done(void *u, int ok, const char *t) {
    (void)u;
    g_text_ok = ok;
    if (t) strlcpy(g_text, t, sizeof g_text);
}

static void step2(void) {
    napple_frontend_show_overlay(g_fe, 0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        CHECK(g_overlay_on == 1 && g_overlay_off == 1, "overlay callbacks %d/%d", g_overlay_on, g_overlay_off);
        napple_text_request q = {.max_length = 5};
        setenv("NAPPLE_SWKBD_ANSWER", "Epona", 1);
        napple_frontend_text_input(g_fe, &q, text_done, NULL);
        CHECK(g_text_ok == 1 && !strcmp(g_text, "Epona"), "scripted swkbd answer");
        setenv("NAPPLE_SWKBD_ANSWER", "toolongvalue", 1);
        g_text_ok = 5;
        napple_frontend_text_input(g_fe, &q, text_done, NULL);
        CHECK(g_text_ok == 0, "invalid scripted answer is a cancel");
        setenv("NAPPLE_SWKBD_ANSWER", "<cancel>", 1);
        g_text_ok = 5;
        napple_frontend_text_input(g_fe, &q, text_done, NULL);
        CHECK(g_text_ok == 0, "cancel");
        unsetenv("NAPPLE_SWKBD_ANSWER");
        char out[16];
        CHECK(napple_frontend_text_input_blocking(g_fe, &q, out, sizeof out) == 0, "blocking call on the main thread refuses (no deadlock)");
        napple_frontend_request_quit(g_fe);
    });
}

static void step1(void) {
    void *layer = napple_frontend_metal_layer(g_fe);
    CHECK(layer != NULL && [(__bridge id)layer isKindOfClass:[CAMetalLayer class]], "metal layer");
    int w = 0, h = 0;
    double sc = 0;
    napple_frontend_drawable_size(g_fe, &w, &h, &sc);
    CHECK(w >= 640 && h >= 360 && sc >= 1, "drawable %dx%d @%.1f", w, h, sc);
    napple_settings s;
    napple_frontend_get_settings(g_fe, &s);
    s.fps_cap = 60;
    s.internal_scale_percent = 200;
    s.master_volume = 0.4f;
    s.vsync = 0;
    strcpy(s.update_version, "1.1.0");
    napple_frontend_set_settings(g_fe, &s);
    char path[200];
    snprintf(path, sizeof path, "%s/settings.json", g_dir);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "settings.json written");
    if (f) fclose(f);
    CHECK(g_changed >= 1, "on_settings_changed fired");
    napple_frontend_get_settings(g_fe, &s);
    CHECK(s.fps_cap == 60 && s.internal_scale_percent == 200, "settings stored");
    napple_manifest_dlc dlc[2] = {{"01007EF00011F001", "Expansion Pass"}, {"01007EF00011F002", "Extra"}};
    napple_manifest_update up[2] = {{"1.1.0", "Update 1.1.0"}, {"1.0.1", ""}};
    napple_manifest m = {
        .title_id = "01007EF00011E000", .title_name = "Test Game", .base_version = "1.0.0", .dlc = dlc, .dlc_count = 2, .updates = up, .update_count = 2};
    napple_frontend_set_manifest(g_fe, &m);
    napple_frontend_show_overlay(g_fe, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ step2(); });
}

int main(void) {
    @autoreleasepool {
        strcpy(g_dir, "/tmp/napple-fe-test-XXXXXX");
        mkdtemp(g_dir);
        napple_input *in = NULL;
        napple_input_create(&(napple_input_config){.config_dir = g_dir}, &in);
        napple_frontend_config c = {
            .title_id = "01007EF00011E000",
            .title_name = "Test Game",
            .config_dir = g_dir,
            .headless = 1,
            .input = in,
            .callbacks = {
                .on_ready = on_ready, .on_settings_changed = on_changed, .on_overlay = on_overlay, .on_quit = on_quit, .on_restart_guest = on_restart}};
        g_fe = napple_frontend_create(&c);
        CHECK(g_fe != NULL, "create");
        dispatch_async(dispatch_get_main_queue(), ^{ step1(); });
        napple_frontend_run(g_fe);
        CHECK(g_ready == 1 && g_quit == 1, "ready %d quit %d", g_ready, g_quit);
        napple_frontend_destroy(g_fe);
        napple_input_destroy(in);
        /* persistence across instances */
        g_fe = napple_frontend_create(&c);
        napple_settings s;
        napple_frontend_get_settings(g_fe, &s);
        CHECK(s.fps_cap == 60 && s.internal_scale_percent == 200 && !strcmp(s.update_version, "1.1.0") && !s.vsync, "settings reloaded");
        napple_frontend_destroy(g_fe);
    }
    printf("{\"test\":\"window\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
