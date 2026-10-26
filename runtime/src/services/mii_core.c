#include "mii_core.h"
#include <string.h>

static char user_name[32] = "Napple";
void mii_set_user_name(const char *utf8) { if (utf8 && *utf8) { strncpy(user_name, utf8, sizeof user_name - 1); user_name[sizeof user_name - 1] = 0; } }

size_t mii_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    size_t n = 0;
    while (*s && n < cap) {
        uint32_t c = (uint8_t)*s++;
        if (c >= 0xF0) { s += 3; c = '?'; }                                  /* outside the BMP: replaced */
        else if (c >= 0xE0 && s[0] && s[1]) { c = ((c & 0x0F) << 12) | (((uint8_t)s[0] & 0x3F) << 6) | ((uint8_t)s[1] & 0x3F); s += 2; }
        else if (c >= 0xC0 && s[0]) { c = ((c & 0x1F) << 6) | ((uint8_t)s[0] & 0x3F); s += 1; }
        else if (c >= 0x80) c = '?';
        out[n++] = (uint16_t)c;
    }
    return n;
}

/* Base appearance shared by every built-in Mii: mid-range values for every slider so the face is valid and neutral. */
static void fill_neutral(MiiCharInfo *m) {
    memset(m, 0, sizeof *m);
    m->height = 64; m->build = 64;
    m->eye_scale = 4; m->eye_aspect = 3; m->eye_rotate = 4; m->eye_x = 2; m->eye_y = 12;
    m->eyebrow_scale = 4; m->eyebrow_aspect = 3; m->eyebrow_rotate = 6; m->eyebrow_x = 2; m->eyebrow_y = 10;
    m->nose_scale = 4; m->nose_y = 9;
    m->mouth_scale = 4; m->mouth_aspect = 3; m->mouth_y = 13;
    m->mustache_scale = 4; m->mustache_y = 10;
    m->glasses_scale = 4; m->glasses_y = 10;
    m->mole_scale = 4; m->mole_x = 2; m->mole_y = 20;
    m->hair_color = 1; m->eye_color = 8; m->eyebrow_color = 1; m->mouth_color = 19;
}

/* A create ID is a UUID: the SDK's CharInfoRaw::Verify (and so CharInfoAccessor::IsValid, which Mario Kart 8 builds a head model from) wants version 4 and the RFC 4122 variant bits. */
static void set_id(MiiCharInfo *m, uint8_t tag, uint32_t index) {
    memcpy(m->create_id, "NappleMii", 9); m->create_id[9] = tag; m->create_id[12] = (uint8_t)index; m->create_id[15] = 1;
    m->create_id[6] = (uint8_t)((m->create_id[6] & 0x0f) | 0x40); m->create_id[8] = (uint8_t)((m->create_id[8] & 0x3f) | 0x80);
}

/* The system's default Mii as the mii service builds it (3DS-era default parameters, colours as the Switch's common colour indices); index picks the colour and gender. */
void mii_build_default(uint32_t index, MiiCharInfo *out) {
    index %= MII_DEFAULT_COUNT;
    fill_neutral(out);
    set_id(out, 'D', index);
    out->favorite_color = (uint8_t)(index * 2);            /* red, yellow, green, sky blue, purple, white */
    out->gender = index >= 3;
    out->hair_type = 33; out->hair_color = 1;
    out->eye_type = 2; out->eye_color = 8; out->eyebrow_type = 6; out->eyebrow_color = 1; out->nose_type = 1; out->mouth_type = 23; out->mouth_color = 19;
    out->beard_color = 8; out->glasses_color = 8;
    mii_utf8_to_utf16("no name", out->name, 10);
}

void mii_build_random(uint32_t age, uint32_t gender, uint32_t race, MiiCharInfo *out) {
    (void)age; (void)race;
    mii_build_default(gender == MII_GENDER_FEMALE ? 3 : 0, out);   /* defaults 0..2 are male, 3..5 female */
    out->gender = gender == MII_GENDER_FEMALE ? MII_GENDER_FEMALE : MII_GENDER_MALE;
    static uint32_t serial; uint32_t n = ++serial; memcpy(out->create_id + 10, &n, 4);   /* fresh ID per random Mii (the title builds twelve) */
}

static void build_user(MiiCharInfo *m) {
    fill_neutral(m);
    set_id(m, 'U', 0);
    m->favorite_color = 4; m->hair_type = 33; m->eye_type = 2; m->nose_type = 1; m->mouth_type = 23; m->faceline_type = 0;
    mii_utf8_to_utf16(user_name, m->name, 10);
}

uint32_t mii_count(uint32_t f) { return ((f & MII_FLAG_DATABASE) ? 1u : 0u) + ((f & MII_FLAG_DEFAULT) ? MII_DEFAULT_COUNT : 0u); }

uint32_t mii_list(uint32_t f, MiiElement *out, uint32_t cap) {
    uint32_t n = 0;
    if ((f & MII_FLAG_DATABASE) && n < cap) { build_user(&out[n].info); out[n].source = MII_SOURCE_DATABASE; n++; }
    if (f & MII_FLAG_DEFAULT) for (uint32_t i = 0; i < MII_DEFAULT_COUNT && n < cap; i++) { mii_build_default(i, &out[n].info); out[n].source = MII_SOURCE_DEFAULT; n++; }
    return n;
}

int mii_find_index(const uint8_t id[16]) {
    MiiCharInfo u; build_user(&u);
    return memcmp(u.create_id, id, 16) == 0 ? 0 : -1;
}

#define IN(v, lo, hi) ((v) >= (lo) && (v) <= (hi))
int mii_char_info_valid(const MiiCharInfo *m) {
    return IN(m->favorite_color, 0, 11) && m->gender <= 1 && m->height <= 127 && m->build <= 127 && IN(m->faceline_type, 0, 11) && IN(m->faceline_color, 0, 5) &&
           IN(m->faceline_wrinkle, 0, 11) && IN(m->faceline_make, 0, 11) && IN(m->hair_type, 0, 131) && IN(m->hair_color, 0, 99) && m->hair_flip <= 1 &&
           IN(m->eye_type, 0, 59) && IN(m->eye_color, 0, 99) && IN(m->eye_scale, 0, 7) && IN(m->eye_aspect, 0, 6) && IN(m->eye_rotate, 0, 7) && IN(m->eye_x, 0, 12) && IN(m->eye_y, 0, 18) &&
           IN(m->eyebrow_type, 0, 23) && IN(m->eyebrow_color, 0, 99) && IN(m->eyebrow_scale, 0, 8) && IN(m->eyebrow_aspect, 0, 6) && IN(m->eyebrow_rotate, 0, 11) && IN(m->eyebrow_x, 0, 12) && IN(m->eyebrow_y, 3, 18) &&
           IN(m->nose_type, 0, 17) && IN(m->nose_scale, 0, 8) && IN(m->nose_y, 0, 18) &&
           IN(m->mouth_type, 0, 35) && IN(m->mouth_color, 0, 99) && IN(m->mouth_scale, 0, 8) && IN(m->mouth_aspect, 0, 6) && IN(m->mouth_y, 0, 18) &&
           IN(m->beard_color, 0, 99) && IN(m->beard_type, 0, 5) && IN(m->mustache_type, 0, 5) && IN(m->mustache_scale, 0, 8) && IN(m->mustache_y, 0, 16) &&
           IN(m->glasses_type, 0, 8) && IN(m->glasses_color, 0, 99) && IN(m->glasses_scale, 0, 7) && IN(m->glasses_y, 0, 20) &&
           IN(m->mole_type, 0, 1) && IN(m->mole_scale, 0, 8) && IN(m->mole_x, 0, 16) && IN(m->mole_y, 0, 30);
}
