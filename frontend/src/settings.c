/* Pure parts of the frontend: settings model + JSON, data paths, text validation, UTF conversion, frame limiter.
   No AppKit here so everything is unit tested headless (tests/test_settings.c). */
#include "napple_frontend.h"
#include "tinyjson.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void napple_settings_default(napple_settings *s) {
    memset(s, 0, sizeof *s);
    s->internal_scale_percent = 100; s->output_width = 1280; s->output_height = 720;
    s->vsync = 1; s->master_volume = 1.0f; s->mute_in_background = 1; s->show_touch_cursor = 1;
    s->supersample_percent = 100; s->filter = 3; s->sharpness = 30; s->frame_gen_mult = 0; s->speed_percent = 100; s->turbo_percent = 300;
}

static int clampi(double v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }
static int dim(double v) { int i = (int)v; return i == 0 ? 0 : clampi(v, 320, 16384); }
static int is_title_id(const char *s) { if (strlen(s) != 16) return 0; for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) return 0; return 1; }

/* Version strings are shown in UI and passed to the runtime: keep them to a safe charset. */
static int safe_version(const char *s) { for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '.' || *s == '-' || *s == '_' || *s == '+')) return 0; return 1; }

void napple_settings_clamp(napple_settings *s) {
    s->internal_scale_percent = clampi(s->internal_scale_percent, 25, 400);
    s->internal_width = dim(s->internal_width); s->internal_height = dim(s->internal_height);
    if (!s->internal_width != !s->internal_height) s->internal_width = s->internal_height = 0;
    s->output_width = s->output_width ? clampi(s->output_width, 320, 16384) : 1280; s->output_height = s->output_height ? clampi(s->output_height, 240, 16384) : 720;
    s->fps_cap = s->fps_cap == 0 ? 0 : clampi(s->fps_cap, 15, 1000);
    s->vsync = s->vsync != 0; s->fullscreen = s->fullscreen != 0; s->mute_in_background = s->mute_in_background != 0; s->show_touch_cursor = s->show_touch_cursor != 0;
    s->aspect = clampi(s->aspect, 0, 6);
    s->render_mode = clampi(s->render_mode, 0, 3); s->supersample_percent = clampi(s->supersample_percent, 100, 400); s->filter = clampi(s->filter, 0, 5);
    s->sharpness = clampi(s->sharpness, 0, 100); s->upscaler = clampi(s->upscaler, 0, 1);
    { int a = s->anisotropy; s->anisotropy = a < 2 ? 0 : a < 4 ? 2 : a < 8 ? 4 : a < 16 ? 8 : 16; }
    s->frame_gen = s->frame_gen != 0; s->frame_gen_mult = s->frame_gen_mult == 0 ? 0 : clampi(s->frame_gen_mult, 2, 8); s->frame_gen_method = clampi(s->frame_gen_method, 0, 4);
    s->show_stats = s->show_stats != 0;
    s->speed_percent = clampi(s->speed_percent, 10, 1000); s->turbo_percent = clampi(s->turbo_percent, 110, 1000);
    s->speed_mode = clampi(s->speed_mode, 0, 2); s->audio_speed_mode = clampi(s->audio_speed_mode, 0, 2);
    if (!(s->master_volume >= 0)) s->master_volume = 0; if (s->master_volume > 1) s->master_volume = 1;
    s->update_version[sizeof s->update_version - 1] = 0;
    if (!safe_version(s->update_version)) s->update_version[0] = 0;
    if (s->disabled_dlc_count < 0) s->disabled_dlc_count = 0;
    if (s->disabled_dlc_count > 32) s->disabled_dlc_count = 32;
    int keep = 0;
    for (int i = 0; i < s->disabled_dlc_count; i++) { s->disabled_dlc[i][19] = 0; if (is_title_id(s->disabled_dlc[i])) { if (keep != i) memcpy(s->disabled_dlc[keep], s->disabled_dlc[i], 20); keep++; } }
    s->disabled_dlc_count = keep;
}

static void put(char *out, size_t cap, size_t *at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(char *out, size_t cap, size_t *at, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(*at < cap ? out + *at : NULL, *at < cap ? cap - *at : 0, fmt, ap);
    va_end(ap); if (n > 0) *at += (size_t)n;
}

size_t napple_settings_to_json(const napple_settings *s, char *out, size_t cap) {
    size_t at = 0;
    put(out, cap, &at, "{\n  \"version\": 1,\n  \"internal_width\": %d,\n  \"internal_height\": %d,\n  \"internal_scale_percent\": %d,\n  \"output_width\": %d,\n  \"output_height\": %d,\n"
        "  \"fps_cap\": %d,\n  \"vsync\": %s,\n  \"fullscreen\": %s,\n  \"aspect\": %d,\n  \"master_volume\": %.3f,\n  \"mute_in_background\": %s,\n  \"show_touch_cursor\": %s,\n"
        "  \"render_mode\": %d,\n  \"supersample_percent\": %d,\n  \"filter\": %d,\n  \"sharpness\": %d,\n  \"upscaler\": %d,\n  \"anisotropy\": %d,\n"
        "  \"frame_gen\": %s,\n  \"frame_gen_mult\": %d,\n  \"frame_gen_method\": %d,\n  \"show_stats\": %s,\n"
        "  \"speed_percent\": %d,\n  \"turbo_percent\": %d,\n  \"speed_mode\": %d,\n  \"audio_speed_mode\": %d,\n  \"update_version\": ",
        s->internal_width, s->internal_height, s->internal_scale_percent, s->output_width, s->output_height, s->fps_cap, s->vsync ? "true" : "false", s->fullscreen ? "true" : "false",
        s->aspect, s->master_volume, s->mute_in_background ? "true" : "false", s->show_touch_cursor ? "true" : "false",
        s->render_mode, s->supersample_percent, s->filter, s->sharpness, s->upscaler, s->anisotropy, s->frame_gen ? "true" : "false", s->frame_gen_mult, s->frame_gen_method,
        s->show_stats ? "true" : "false", s->speed_percent, s->turbo_percent, s->speed_mode, s->audio_speed_mode);
    tj_put_str(out, cap, &at, s->update_version);
    put(out, cap, &at, ",\n  \"disabled_dlc\": [");
    for (int i = 0; i < s->disabled_dlc_count; i++) { put(out, cap, &at, "%s", i ? ", " : ""); tj_put_str(out, cap, &at, s->disabled_dlc[i]); }
    put(out, cap, &at, "]\n}\n");
    return at;
}

int napple_settings_from_json(const char *json, size_t len, napple_settings *out) {
    tj_node pool[192];
    if (!json || len > 65536) return -1;
    int root = tj_parse(json, len, pool, sizeof pool / sizeof *pool);
    if (root < 0 || pool[root].type != TJ_OBJ) return -1;
    const tj_node *r = &pool[root];
    if (tj_num(tj_get(pool, r, "version"), 1) != 1) return -2;
    napple_settings s; napple_settings_default(&s);
    #define NUM(field, key) do { const tj_node *n = tj_get(pool, r, key); if (n) { if (n->type != TJ_NUM) return -1; s.field = (int)n->num; } } while (0)
    NUM(internal_width, "internal_width"); NUM(internal_height, "internal_height"); NUM(internal_scale_percent, "internal_scale_percent");
    NUM(output_width, "output_width"); NUM(output_height, "output_height"); NUM(fps_cap, "fps_cap"); NUM(aspect, "aspect");
    NUM(render_mode, "render_mode"); NUM(supersample_percent, "supersample_percent"); NUM(filter, "filter"); NUM(sharpness, "sharpness"); NUM(upscaler, "upscaler");
    NUM(anisotropy, "anisotropy"); NUM(frame_gen_mult, "frame_gen_mult"); NUM(frame_gen_method, "frame_gen_method"); NUM(speed_percent, "speed_percent");
    NUM(turbo_percent, "turbo_percent"); NUM(speed_mode, "speed_mode"); NUM(audio_speed_mode, "audio_speed_mode");
    #undef NUM
    /* files from before render_mode existed: a custom size meant "fixed", a scale other than 100 % meant "scale factor" */
    if (!tj_get(pool, r, "render_mode")) s.render_mode = (s.internal_width && s.internal_height) ? 3 : s.internal_scale_percent != 100 ? 1 : 0;
    #define FLAG(field, key) do { const tj_node *n = tj_get(pool, r, key); if (n) { if (n->type != TJ_BOOL) return -1; s.field = n->b; } } while (0)
    FLAG(vsync, "vsync"); FLAG(fullscreen, "fullscreen"); FLAG(mute_in_background, "mute_in_background"); FLAG(show_touch_cursor, "show_touch_cursor"); FLAG(frame_gen, "frame_gen"); FLAG(show_stats, "show_stats");
    #undef FLAG
    const tj_node *mv = tj_get(pool, r, "master_volume"); if (mv) { if (mv->type != TJ_NUM) return -1; s.master_volume = (float)mv->num; }
    const tj_node *uv = tj_get(pool, r, "update_version"); if (uv) { if (uv->type != TJ_STR) return -1; tj_str(uv, s.update_version, sizeof s.update_version); }
    const tj_node *dl = tj_get(pool, r, "disabled_dlc");
    if (dl) {
        if (dl->type != TJ_ARR) return -1;
        for (const tj_node *e = tj_at(pool, dl, 0); e && s.disabled_dlc_count < 32; ) {
            if (e->type != TJ_STR) return -1;
            tj_str(e, s.disabled_dlc[s.disabled_dlc_count++], 20);
            e = e->next >= 0 ? &pool[e->next] : NULL;
        }
    }
    napple_settings_clamp(&s);
    *out = s; return 0;
}

int napple_data_path(const char *config_dir, const char *title_id, const char *file, char *out, size_t cap) {
    if (!out || !file || strchr(file, '/') || strstr(file, "..")) return -1;
    int n;
    if (config_dir && *config_dir) n = snprintf(out, cap, "%s/%s", config_dir, file);
    else {
        char id[40]; size_t k = 0;
        for (const char *p = title_id; p && *p && k < 20; p++) if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) id[k++] = *p;
        id[k] = 0; if (!k) strcpy(id, "default");
        const char *home = getenv("HOME");
        n = snprintf(out, cap, "%s/Library/Application Support/Napple/%s/%s", home && *home ? home : "/tmp", id, file);
    }
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* ---- text ---- */
static size_t cp_len(const char *s) { size_t n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }
int napple_text_validate(const napple_text_request *q, const char *s) {
    if (!q || !s) return 0;
    size_t n = cp_len(s), max = q->max_length ? q->max_length : 500;
    if (n > max || n < q->min_length) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return 0;
        if ((q->disabled_keys & 2) && c == ' ') return 0;
        if ((q->disabled_keys & 4) && c == '@') return 0;
        if ((q->disabled_keys & 8) && c == '%') return 0;
        if ((q->disabled_keys & 16) && c == '/') return 0;
        if ((q->disabled_keys & 32) && c == '\\') return 0;
        if ((q->disabled_keys & 64) && c >= '0' && c <= '9') return 0;
        if (q->numeric && !(c >= '0' && c <= '9') && c != '.' && c != '-') return 0;
    }
    return 1;
}

size_t napple_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0; if (!cap) return 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        char b[4]; size_t k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { b[0] = (char)(0xf0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 0x3f)); b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k); o += k;
    }
    out[o] = 0; return o;
}

size_t napple_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    size_t o = 0; const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c; int k;
        if (*p < 0x80) { c = *p; k = 1; } else if ((*p & 0xe0) == 0xc0) { c = *p & 0x1f; k = 2; } else if ((*p & 0xf0) == 0xe0) { c = *p & 0x0f; k = 3; } else if ((*p & 0xf8) == 0xf0) { c = *p & 0x07; k = 4; } else { c = 0xfffd; k = 1; }
        int ok = 1; for (int i = 1; i < k; i++) { if ((p[i] & 0xc0) != 0x80) { ok = 0; break; } c = c << 6 | (p[i] & 0x3f); }
        if (!ok || (k == 2 && c < 0x80) || (k == 3 && c < 0x800) || (k == 4 && (c < 0x10000 || c > 0x10ffff)) || (c >= 0xd800 && c < 0xe000)) { c = 0xfffd; k = ok ? k : 1; }
        p += k;
        size_t need = c >= 0x10000 ? 2 : 1;
        if (o + need > cap) break;
        if (need == 2) { c -= 0x10000; out[o++] = (uint16_t)(0xd800 + (c >> 10)); out[o++] = (uint16_t)(0xdc00 + (c & 0x3ff)); } else out[o++] = (uint16_t)c;
    }
    return o;
}

/* ---- frame limiter ---- */
static uint64_t mono_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void napple_frame_limiter_init(napple_frame_limiter *l, int fps) { l->next_ns = 0; l->period_ns = fps > 0 ? 1000000000ull / (uint64_t)fps : 0; }
uint64_t napple_frame_limiter_step(napple_frame_limiter *l, uint64_t now) {
    if (!l->period_ns) return 0;
    if (!l->next_ns) { l->next_ns = now + l->period_ns; return l->period_ns; }
    uint64_t wait = l->next_ns > now ? l->next_ns - now : 0;
    l->next_ns += l->period_ns;
    if (now > l->next_ns + 4 * l->period_ns) l->next_ns = now + l->period_ns;   /* far behind (stall): resynchronise */
    return wait;
}
uint64_t napple_frame_limiter_wait(napple_frame_limiter *l) {
    uint64_t now = mono_ns(), w = napple_frame_limiter_step(l, now);
    if (w > 300000) { struct timespec d = {0, (long)(w - 200000)}; nanosleep(&d, NULL); }
    while (mono_ns() < now + w) {}
    return w;
}

/* Effective game speed (percent): the hold key wins over the toggles, slow motion wins over the base speed; a toggled turbo uses the turbo speed.
   Pure so the hot key logic is unit tested without a window. */
int napple_effective_speed_percent(const napple_settings *s, int turbo_held, int turbo_toggled, int slow_toggled) {
    int v = s->speed_percent;
    if (slow_toggled) v = 50;
    if (turbo_held || turbo_toggled) v = s->turbo_percent;
    return clampi(v, 10, 1000);
}
