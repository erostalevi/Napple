/* Unit tests of the overlay's pure helpers (src/overlay_logic.c). */
#include <stdio.h>
#include <string.h>
#include "overlay_logic.h"

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
    char b[64];
    CHECK(fe_tab_wrap(0, -1, 5) == 4 && fe_tab_wrap(4, 1, 5) == 0 && fe_tab_wrap(2, 0, 5) == 2 && fe_tab_wrap(1, -7, 5) == 4 && fe_tab_wrap(3, 2, 0) == 0, "tab wrap");
    fe_format_scale(100, b, sizeof b);
    CHECK(!strcmp(b, "100% (native)"), "scale native: %s", b);
    fe_format_scale(150, b, sizeof b);
    CHECK(!strcmp(b, "150%"), "scale 150: %s", b);
    fe_format_resolution(1920, 1080, b, sizeof b);
    CHECK(!strcmp(b, "1920 x 1080 (1080p)"), "res 1080p: %s", b);
    fe_format_resolution(3840, 2160, b, sizeof b);
    CHECK(!strcmp(b, "3840 x 2160 (4K)"), "res 4K: %s", b);
    fe_format_resolution(2560, 1600, b, sizeof b);
    CHECK(!strcmp(b, "2560 x 1600"), "res 16:10 has no class: %s", b);
    fe_format_speed(100, b, sizeof b);
    CHECK(!strcmp(b, "1x"), "speed 1x: %s", b);
    fe_format_speed(50, b, sizeof b);
    CHECK(!strcmp(b, "0.5x"), "speed 0.5x: %s", b);
    fe_format_speed(250, b, sizeof b);
    CHECK(!strcmp(b, "2.5x"), "speed 2.5x: %s", b);
    fe_format_speed(25, b, sizeof b);
    CHECK(!strcmp(b, "0.25x"), "speed 0.25x: %s", b);
    CHECK(fe_parse_percent("150", 10, 800, 100) == 150 && fe_parse_percent(" 200 % ", 10, 800, 100) == 200, "parse percent");
    CHECK(fe_parse_percent("5", 10, 800, 100) == 10 && fe_parse_percent("9000", 10, 800, 100) == 800, "parse percent clamps");
    CHECK(fe_parse_percent("abc", 10, 800, 100) == 100 && fe_parse_percent("", 10, 800, 100) == 100 && fe_parse_percent("12x", 10, 800, 100) == 100 &&
              fe_parse_percent(NULL, 10, 800, 77) == 77 && fe_parse_percent("-5", 10, 800, 100) == 100,
          "parse percent rejects junk");

    napple_settings s;
    napple_settings_default(&s);
    CHECK(!fe_dlc_is_disabled(&s, "01007EF00011F001"), "dlc enabled by default");
    CHECK(fe_dlc_set_enabled(&s, "01007EF00011F001", 0) == 1 && fe_dlc_is_disabled(&s, "01007ef00011f001") && s.disabled_dlc_count == 1, "disable (case-insensitive)");
    CHECK(fe_dlc_set_enabled(&s, "01007EF00011F001", 0) == 0 && s.disabled_dlc_count == 1, "disable twice is a no-op");
    CHECK(fe_dlc_set_enabled(&s, "01007EF00011F002", 0) == 1 && s.disabled_dlc_count == 2, "second disabled");
    CHECK(fe_dlc_set_enabled(&s, "01007EF00011F001", 1) == 1 && !fe_dlc_is_disabled(&s, "01007EF00011F001") && fe_dlc_is_disabled(&s, "01007EF00011F002") &&
              s.disabled_dlc_count == 1,
          "re-enable removes only that id");
    CHECK(fe_dlc_set_enabled(&s, "01007EF00011F009", 1) == 0, "enabling an enabled id is a no-op");
    napple_settings f;
    napple_settings_default(&f);
    for (int i = 0; i < 32; i++) {
        char id[20];
        snprintf(id, sizeof id, "0100000000000%03X", i);
        fe_dlc_set_enabled(&f, id, 0);
    }
    CHECK(f.disabled_dlc_count == 32 && fe_dlc_set_enabled(&f, "01000000000FFFFF", 0) == 0, "the 33rd id is refused");

    fe_update_choose(&s, "1.1.0");
    CHECK(!strcmp(s.update_version, "1.1.0"), "update chosen");
    fe_update_choose(&s, NULL);
    CHECK(!s.update_version[0], "update back to latest");

    napple_settings r;
    napple_settings_default(&r);
    r.master_volume = 0.3f;
    r.mute_in_background = 1;
    r.panel_passthrough = 0;
    strcpy(r.update_version, "1.2.0");
    fe_dlc_set_enabled(&r, "01007EF00011F001", 0);
    r.render_mode = 3;
    r.internal_width = 3840;
    r.internal_height = 2160;
    r.upscaler = 2;
    r.speed_percent = 400;
    r.show_stats = 1;
    r.fullscreen = 1;
    fe_reset_display_speed(&r);
    napple_settings d;
    napple_settings_default(&d);
    CHECK(r.render_mode == d.render_mode && r.upscaler == d.upscaler && r.speed_percent == d.speed_percent && r.show_stats == d.show_stats && r.fullscreen == d.fullscreen &&
              r.internal_width == d.internal_width,
          "reset restores video and speed defaults");
    CHECK(r.master_volume == 0.3f && r.mute_in_background == 1 && r.panel_passthrough == 0 && !strcmp(r.update_version, "1.2.0") && r.disabled_dlc_count == 1 &&
              fe_dlc_is_disabled(&r, "01007EF00011F001"),
          "reset keeps audio, panel and game choices");

    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
