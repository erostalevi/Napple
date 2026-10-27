#include "../src/internal.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

int main(void) {
    napple_settings d, s, r;
    napple_settings_default(&d);
    char json[8192];
    size_t n = napple_settings_to_json(&d, json, sizeof json);
    CHECK(napple_settings_from_json(json, n, &r) == 0 && !memcmp(&d, &r, sizeof d), "default round trip");
    s = d;
    s.internal_width = 1920;
    s.internal_height = 1080;
    s.fps_cap = 144;
    s.vsync = 0;
    s.fullscreen = 1;
    s.master_volume = 0.5f;
    s.aspect = 2;
    strcpy(s.update_version, "1.2.1");
    strcpy(s.disabled_dlc[0], "01007EF00011F001");
    strcpy(s.disabled_dlc[1], "01007EF00011F002");
    s.disabled_dlc_count = 2;
    n = napple_settings_to_json(&s, json, sizeof json);
    CHECK(napple_settings_from_json(json, n, &r) == 0 && !memcmp(&s, &r, sizeof s), "full round trip");
    /* clamping */
    const char *cl = "{\"fps_cap\":3,\"internal_scale_percent\":9999,\"master_volume\":7,\"aspect\":9,\"output_width\":1}";
    CHECK(napple_settings_from_json(cl, strlen(cl), &r) == 0 && r.fps_cap == 15 && r.internal_scale_percent == 400 && r.master_volume == 1 && r.aspect == 6 &&
              r.output_width == 320,
          "clamped values");
    /* fps_mode migration: old fps_cap paced the guest; it becomes a present rate plus an explicit Speed */
    {
        const char *a = "{\"fps_cap\":120}", *b = "{\"fps_cap\":0}", *c2 = "{\"fps_cap\":1000}", *e = "{\"fps_cap\":60}", *f = "{\"fps_cap\":90,\"speed_percent\":100}";
        CHECK(napple_settings_from_json(a, strlen(a), &r) == 0 && r.fps_mode == 2 && r.fps_cap == 120 && r.speed_percent == 200, "old fps_cap 120 -> custom 120 at speed 200%%");
        CHECK(napple_settings_from_json(b, strlen(b), &r) == 0 && r.fps_mode == 0 && r.speed_percent == 100, "old fps_cap 0 -> game pacing");
        CHECK(napple_settings_from_json(c2, strlen(c2), &r) == 0 && r.fps_mode == 3 && r.speed_percent == 100, "old fps_cap 1000 -> uncapped, speed unchanged");
        CHECK(napple_settings_from_json(e, strlen(e), &r) == 0 && r.fps_mode == 2 && r.speed_percent == 100, "old fps_cap 60 -> custom 60 at 100%%");
        CHECK(napple_settings_from_json(f, strlen(f), &r) == 0 && r.fps_mode == 2 && r.speed_percent == 100, "explicit speed is kept");
        s = d;
        for (int i = 0; i < NAPPLE_FPS_CHOICES; i++) {
            napple_fps_choice_apply(&s, i);
            CHECK(napple_fps_choice(&s) == i, "fps menu choice %d round trips", i);
        }
        napple_fps_choice_apply(&s, 5);
        CHECK(s.fps_mode == 2 && s.fps_cap == 120, "menu 120 Hz = custom 120");
        s = d;
        s.fps_mode = 1;
        n = napple_settings_to_json(&s, json, sizeof json);
        CHECK(napple_settings_from_json(json, n, &r) == 0 && r.fps_mode == 1, "fps_mode round trip");
    }
    /* display / speed fields */
    s = d;
    s.render_mode = 2;
    s.supersample_percent = 200;
    s.filter = 4;
    s.sharpness = 55;
    s.upscaler = 1;
    s.anisotropy = 8;
    s.frame_gen = 1;
    s.frame_gen_mult = 4;
    s.frame_gen_method = 2;
    s.show_stats = 1;
    s.stats_corner = 3;
    s.stats_size = 2;
    s.stats_opacity = 35;
    s.metal_hud = 1;
    s.panel_passthrough = 0;
    s.speed_percent = 250;
    s.turbo_percent = 500;
    s.speed_mode = 1;
    s.audio_speed_mode = 2;
    s.aspect = 5;
    n = napple_settings_to_json(&s, json, sizeof json);
    CHECK(napple_settings_from_json(json, n, &r) == 0 && !memcmp(&s, &r, sizeof s), "display / speed round trip");
    const char *cl2 =
        "{\"render_mode\":9,\"supersample_percent\":50,\"filter\":-4,\"sharpness\":900,\"anisotropy\":5,\"frame_gen_mult\":1,\"frame_gen_method\":8,\"speed_percent\":1,\"turbo_percent\":5000,\"speed_mode\":7,\"audio_speed_mode\":-1}";
    CHECK(napple_settings_from_json(cl2, strlen(cl2), &r) == 0 && r.render_mode == 3 && r.supersample_percent == 100 && r.filter == 0 && r.sharpness == 100 &&
              r.anisotropy == 4 && r.frame_gen_mult == 2 && r.frame_gen_method == 4 && r.speed_percent == 10 && r.turbo_percent == 1000 && r.speed_mode == 2 &&
              r.audio_speed_mode == 0,
          "display / speed values clamped");
    const char *cl3 = "{\"stats_corner\":9,\"stats_size\":-2,\"stats_opacity\":1,\"metal_hud\":true}";
    CHECK(napple_settings_from_json(cl3, strlen(cl3), &r) == 0 && r.stats_corner == 3 && r.stats_size == 0 && r.stats_opacity == 10 && r.metal_hud && r.panel_passthrough,
          "stats / panel settings clamped, passthrough defaults on");
    /* settings files written before render_mode existed keep their meaning */
    const char *old1 = "{\"internal_width\":2560,\"internal_height\":1440}", *old2 = "{\"internal_scale_percent\":200}", *old3 = "{}";
    CHECK(napple_settings_from_json(old1, strlen(old1), &r) == 0 && r.render_mode == 3, "old custom size -> fixed");
    CHECK(napple_settings_from_json(old2, strlen(old2), &r) == 0 && r.render_mode == 1, "old scale -> scale factor");
    CHECK(napple_settings_from_json(old3, strlen(old3), &r) == 0 && r.render_mode == 0 && r.speed_percent == 100 && r.filter == 3 && !r.frame_gen,
          "defaults for a minimal file");
    /* effective speed: hold wins over toggles; slow motion beats the base speed */
    s = d;
    s.speed_percent = 150;
    s.turbo_percent = 400;
    CHECK(napple_effective_speed_percent(&s, 0, 0, 0) == 150 && napple_effective_speed_percent(&s, 1, 0, 0) == 400 &&
              napple_effective_speed_percent(&s, 0, 1, 0) == 400 && napple_effective_speed_percent(&s, 0, 0, 1) == 50 &&
              napple_effective_speed_percent(&s, 1, 0, 1) == 400,
          "effective speed");
    const char *evil = "{\"update_version\":\"../../etc\",\"disabled_dlc\":[\"zzz\",\"01007EF00011F001\"]}";
    CHECK(napple_settings_from_json(evil, strlen(evil), &r) == 0 && r.update_version[0] == 0 && r.disabled_dlc_count == 1,
          "unsafe version dropped, bad dlc id dropped");
    const char *bad[] = {"", "[]", "{\"fps_cap\":\"x\"}", "{\"vsync\":1}", "{\"version\":3}", "{\"disabled_dlc\":[1]}"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(napple_settings_from_json(bad[i], strlen(bad[i]), &r) != 0, "bad %u", i);
    srand(3);
    n = napple_settings_to_json(&s, json, sizeof json);
    char buf[8192];
    for (int i = 0; i < 20000; i++) {
        memcpy(buf, json, n);
        for (int k = 0; k < 4; k++) buf[rand() % n] = (char)rand();
        napple_settings_from_json(buf, rand() % (n + 1), &r);
    }
    CHECK(1, "fuzz");
    /* paths */
    char p[512];
    CHECK(napple_data_path("/tmp/x", "0100", "settings.json", p, sizeof p) == 0 && !strcmp(p, "/tmp/x/settings.json"), "config_dir path");
    CHECK(napple_data_path(NULL, "../../01007EF00011E000", "settings.json", p, sizeof p) == 0 && strstr(p, "Napple/01007EF00011E000/settings.json") &&
              !strstr(p, ".."),
          "title id sanitised: %s", p);
    CHECK(napple_data_path(NULL, "01", "../x", p, sizeof p) != 0 && napple_data_path(NULL, "01", "a/b", p, sizeof p) != 0, "file name traversal rejected");
    /* text */
    napple_text_request q = {.max_length = 9, .disabled_keys = 4 | 8 | 32};
    CHECK(napple_text_validate(&q, "Epona") && !napple_text_validate(&q, "a@b") && !napple_text_validate(&q, "100%") && !napple_text_validate(&q, "ab\\c") &&
              !napple_text_validate(&q, "0123456789"),
          "text validation");
    CHECK(napple_text_validate(&q, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a\xe3\x81\x8b\xe3\x81\x8d\xe3\x81\x8f\xe3\x81\x91") &&
              !napple_text_validate(&q,
                                    "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a\xe3\x81\x8b\xe3\x81\x8d\xe3\x81\x8f\xe3\x81\x91\xe3\x81\x93"),
          "limit counts characters, not bytes");
    q = (napple_text_request){.numeric = 1, .min_length = 2};
    CHECK(napple_text_validate(&q, "12") && !napple_text_validate(&q, "1") && !napple_text_validate(&q, "1a"), "numeric and min length");
    CHECK(!napple_text_validate(&q, "1\n2"), "control characters rejected");
    /* utf */
    uint16_t u16[16];
    char u8[64];
    size_t k = napple_utf8_to_utf16("A\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80", u16, 16);
    CHECK(k == 5 && u16[0] == 'A' && u16[1] == 0xe9 && u16[2] == 0x3042 && u16[3] == 0xd83d && u16[4] == 0xde00, "utf8->utf16 incl. surrogates");
    CHECK(napple_utf16_to_utf8(u16, k, u8, sizeof u8) == 10 && !strcmp(u8, "A\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80"), "utf16->utf8 round trip");
    k = napple_utf8_to_utf16("\xff\xc0\x80x", u16, 16);
    CHECK(k >= 2 && u16[k - 1] == 'x' && u16[0] == 0xfffd, "malformed utf8 -> U+FFFD");
    uint16_t lone[2] = {0xd800, 0};
    napple_utf16_to_utf8(lone, 1, u8, sizeof u8);
    CHECK(!strcmp(u8, "\xef\xbf\xbd"), "lone surrogate");
    CHECK(napple_utf8_to_utf16("abc", u16, 2) == 2, "capacity respected");
    CHECK(napple_utf16_to_utf8((uint16_t[]){0x3042, 0}, 1, u8, 3) == 0, "no partial character written");
    /* limiter */
    napple_frame_limiter l;
    napple_frame_limiter_init(&l, 60);
    uint64_t t = 1000, w = napple_frame_limiter_step(&l, t);
    CHECK(w == 16666666, "first frame waits one period");
    t += 16666666;
    w = napple_frame_limiter_step(&l, t);
    CHECK(w == 0 || w < 2, "on time: no wait (%llu)", (unsigned long long)w);
    t += 5000000;
    w = napple_frame_limiter_step(&l, t);
    CHECK(w > 10000000 && w < 12000000, "fast frame waits the rest (%llu)", (unsigned long long)w);
    t += 500000000;
    napple_frame_limiter_step(&l, t);
    w = napple_frame_limiter_step(&l, t + 1000);
    CHECK(w > 10000000, "resync after a stall (%llu)", (unsigned long long)w);
    napple_frame_limiter_init(&l, 0);
    CHECK(napple_frame_limiter_step(&l, 5) == 0, "uncapped never waits");
    napple_frame_limiter_init(&l, 30);
    uint64_t total = 0;
    t = 0;
    for (int i = 0; i < 300; i++) {
        w = napple_frame_limiter_step(&l, t + 1000);
        t += 1000 + w;
        total = t;
    }
    CHECK(llabs((long long)total - 10000000000ll) < 100000000, "300 frames at 30 fps take 10 s (%llu ns)", (unsigned long long)total);
    printf("{\"test\":\"settings\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
