#include "overlay_logic.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int fe_tab_wrap(int cur, int delta, int count) {
    if (count <= 0) return 0;
    int n = (cur + delta) % count;
    return n < 0 ? n + count : n;
}

void fe_format_scale(int percent, char *out, size_t cap) {
    if (percent == 100) snprintf(out, cap, "100%% (native)");
    else snprintf(out, cap, "%d%%", percent);
}

static const char *res_class(int w, int h) {
    if (w * 9 != h * 16) return NULL;
    switch (h) {
        case 540: return "qHD";
        case 720: return "720p";
        case 1080: return "1080p";
        case 1440: return "1440p";
        case 2160: return "4K";
        case 4320: return "8K";
        default: return NULL;
    }
}
void fe_format_resolution(int w, int h, char *out, size_t cap) {
    const char *c = res_class(w, h);
    if (c) snprintf(out, cap, "%d x %d (%s)", w, h, c);
    else snprintf(out, cap, "%d x %d", w, h);
}

void fe_format_speed(int percent, char *out, size_t cap) {
    if (percent % 100 == 0) snprintf(out, cap, "%dx", percent / 100);
    else if (percent % 10 == 0) snprintf(out, cap, "%d.%dx", percent / 100, (percent % 100) / 10);
    else snprintf(out, cap, "%d.%02dx", percent / 100, percent % 100);
}

int fe_parse_percent(const char *text, int lo, int hi, int fallback) {
    if (!text) return fallback;
    while (isspace((unsigned char)*text)) text++;
    if (!isdigit((unsigned char)*text)) return fallback;
    char *end = NULL;
    long v = strtol(text, &end, 10);
    while (end && isspace((unsigned char)*end)) end++;
    if (end && *end == '%') end++;
    while (end && isspace((unsigned char)*end)) end++;
    if (end && *end) return fallback;
    return v < lo ? lo : v > hi ? hi : (int)v;
}

static int dlc_index(const napple_settings *s, const char *id) {
    for (int i = 0; i < s->disabled_dlc_count; i++)
        if (!strcasecmp(s->disabled_dlc[i], id)) return i;
    return -1;
}
int fe_dlc_is_disabled(const napple_settings *s, const char *id) { return id && dlc_index(s, id) >= 0; }
int fe_dlc_set_enabled(napple_settings *s, const char *id, int enabled) {
    if (!id || !*id) return 0;
    int k = dlc_index(s, id);
    if (enabled && k >= 0) {
        for (int i = k; i + 1 < s->disabled_dlc_count; i++) memcpy(s->disabled_dlc[i], s->disabled_dlc[i + 1], sizeof s->disabled_dlc[0]);
        s->disabled_dlc_count--;
        return 1;
    }
    if (!enabled && k < 0 && s->disabled_dlc_count < 32) {
        strlcpy(s->disabled_dlc[s->disabled_dlc_count++], id, sizeof s->disabled_dlc[0]);
        return 1;
    }
    return 0;
}

void fe_update_choose(napple_settings *s, const char *version) {
    if (!version || !*version) s->update_version[0] = 0;
    else strlcpy(s->update_version, version, sizeof s->update_version);
}

void fe_reset_display_speed(napple_settings *s) {
    napple_settings keep = *s, d;
    napple_settings_default(&d);
    d.master_volume = keep.master_volume;
    d.mute_in_background = keep.mute_in_background;
    d.panel_passthrough = keep.panel_passthrough;
    memcpy(d.update_version, keep.update_version, sizeof d.update_version);
    memcpy(d.disabled_dlc, keep.disabled_dlc, sizeof d.disabled_dlc);
    d.disabled_dlc_count = keep.disabled_dlc_count;
    *s = d;
}
