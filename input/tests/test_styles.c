/* GameCube / Palma / NES (Lark) npad styles: selection by the title's supported set and the player's preference, the lifo each style reports
   through, attributes, binding JSON, and that a style the title does not support is never chosen. */
#include "check.h"
#include "../src/internal.h"
#include "napple_input.h"
#include <unistd.h>

static uint32_t r32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t r64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint64_t tick = 1000000000ull;
static uint32_t style_with(napple_input *in, uint32_t supported, int pref) {
    napple_bindings bd;
    napple_bindings_default(&bd);
    bd.style = pref;
    napple_input_set_bindings(in, &bd);
    napple_input_set_supported_styles(in, supported);
    napple_input_update(in, tick += 8000000ull);
    return napple_input_active_style(in, 0);
}

int main(void) {
    char dir[] = "/tmp/napple-styles-test-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    uint8_t *shm = aligned_alloc(16384, NAPPLE_HID_SHM_SIZE);
    napple_input_config c = {.title_id = "0100ABCD12345678", .config_dir = dir, .shm = shm, .use_game_controller = 0};
    napple_input *in;
    CHECK(napple_input_create(&c, &in) == 0, "create");
    napple_hid_layout l;
    napple_hid_layout_for(0, &l);
    napple_input_focus(in, 1);

    const uint32_t standard = NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_JOY_DUAL | NAPPLE_STYLE_HANDHELD;
    const uint32_t nes = NAPPLE_STYLE_LARK_NES_LEFT | NAPPLE_STYLE_LARK_NES_RIGHT;
    CHECK(style_with(in, standard | NAPPLE_STYLE_GC | NAPPLE_STYLE_PALMA | nes, 0) == NAPPLE_STYLE_FULL_KEY, "auto still prefers the Pro Controller");
    CHECK(style_with(in, standard | NAPPLE_STYLE_GC, NAPPLE_STYLE_PREF_GC) == NAPPLE_STYLE_GC, "GameCube preference wins when supported");
    CHECK(style_with(in, standard, NAPPLE_STYLE_PREF_GC) == NAPPLE_STYLE_FULL_KEY, "GameCube preference ignored when the title does not support it");
    CHECK(style_with(in, standard | NAPPLE_STYLE_PALMA, NAPPLE_STYLE_PREF_PALMA) == NAPPLE_STYLE_PALMA, "Palma preference");
    CHECK(style_with(in, standard | nes, NAPPLE_STYLE_PREF_NES) == NAPPLE_STYLE_LARK_NES_LEFT, "NES preference picks the first supported Lark style");
    CHECK(style_with(in, NAPPLE_STYLE_LARK_NES_RIGHT | NAPPLE_STYLE_LARK_HVC_RIGHT, NAPPLE_STYLE_PREF_NES) == NAPPLE_STYLE_LARK_HVC_RIGHT,
          "lowest supported Lark bit");
    /* titles that support only the new styles */
    CHECK(style_with(in, NAPPLE_STYLE_GC, 0) == NAPPLE_STYLE_GC, "GameCube-only title");
    CHECK(style_with(in, NAPPLE_STYLE_PALMA, 0) == NAPPLE_STYLE_PALMA, "Palma-only title");
    CHECK(style_with(in, nes, 0) == NAPPLE_STYLE_LARK_NES_LEFT, "NES-only title");
    CHECK(style_with(in, NAPPLE_STYLE_GC | NAPPLE_STYLE_PALMA, 0) == NAPPLE_STYLE_GC, "GameCube before Palma");

    /* GameCube reports through the FullKey lifo with the wired attribute, style bit set in the npad header */
    napple_input_key(in, 37, 1); /* A */
    style_with(in, NAPPLE_STYLE_GC, 0);
    const uint8_t *e = napple_hid_lifo_latest(shm, l.npad_base + l.npad_lifo[0], 0x28);
    CHECK(r32(shm + l.npad_base) == NAPPLE_STYLE_GC, "npad header carries the GameCube style bit");
    CHECK(e && (r64(e + 8) & NAPPLE_BTN_MASK(NAPPLE_BTN_A)) && r32(e + 0x20) == (NAPPLE_ATTR_CONNECTED | NAPPLE_ATTR_WIRED),
          "GameCube state in the FullKey lifo, wired (%#x)", e ? r32(e + 0x20) : 0);
    /* Palma has its own lifo */
    style_with(in, NAPPLE_STYLE_PALMA, 0);
    e = napple_hid_lifo_latest(shm, l.npad_base + l.npad_lifo[5], 0x28);
    CHECK(e && (r64(e + 8) & NAPPLE_BTN_MASK(NAPPLE_BTN_A)), "Palma state in the Palma lifo");
    /* NES left: left-connected attribute */
    style_with(in, NAPPLE_STYLE_LARK_NES_LEFT, 0);
    e = napple_hid_lifo_latest(shm, l.npad_base + l.npad_lifo[0], 0x28);
    CHECK(e && r32(e + 0x20) == (NAPPLE_ATTR_CONNECTED | NAPPLE_ATTR_LEFT_CONNECTED), "NES left pad: connected + left attribute");
    napple_input_key(in, 37, 0);

    /* binding JSON round trip and validation */
    for (int p = NAPPLE_STYLE_PREF_AUTO; p < NAPPLE_STYLE_PREF_COUNT; p++) {
        napple_bindings b, r;
        char json[8192];
        napple_bindings_default(&b);
        b.style = p;
        size_t n = napple_bindings_to_json(&b, json, sizeof json);
        CHECK(napple_bindings_from_json(json, n, &r) == 0 && r.style == p, "style %d round trips through JSON", p);
    }
    napple_bindings bad;
    napple_bindings_default(&bad);
    bad.style = NAPPLE_STYLE_PREF_COUNT;
    CHECK(napple_bindings_validate(&bad) != 0, "unknown style rejected");
    CHECK(napple_bindings_from_json("{\"style\":\"gc\"}", 14, &bad) == 0 && bad.style == NAPPLE_STYLE_PREF_GC, "\"gc\" parses");

    /* the writers stay in bounds for every style bit */
    uint8_t *tmp = malloc(NAPPLE_HID_SHM_SIZE);
    napple_hid_init(tmp, &l);
    napple_npad_state st = {0};
    for (uint32_t s = 1; s < (1u << 20); s <<= 1)
        for (int slot = 0; slot < 10; slot++) napple_hid_push_npad(tmp, &l, slot, s, &st);
    CHECK(1, "no out-of-bounds writes for any style bit");
    free(tmp);

    napple_input_destroy(in);
    free(shm);
    printf("{\"test\":\"styles\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
