#include "mii_core.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static char user_name[32] = "Napple";
void mii_set_user_name(const char *utf8) {
    if (utf8 && *utf8) {
        strncpy(user_name, utf8, sizeof user_name - 1);
        user_name[sizeof user_name - 1] = 0;
    }
}

size_t mii_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    size_t n = 0;
    while (*s && n < cap) {
        uint32_t c = (uint8_t)*s++;
        if (c >= 0xF0) {
            s += 3;
            c = '?';
        } /* outside the BMP: replaced */
        else if (c >= 0xE0 && s[0] && s[1]) {
            c = ((c & 0x0F) << 12) | (((uint8_t)s[0] & 0x3F) << 6) | ((uint8_t)s[1] & 0x3F);
            s += 2;
        } else if (c >= 0xC0 && s[0]) {
            c = ((c & 0x1F) << 6) | ((uint8_t)s[0] & 0x3F);
            s += 1;
        } else if (c >= 0x80) c = '?';
        out[n++] = (uint16_t)c;
    }
    return n;
}

/* ---- create ids ------------------------------------------------------------------------------------------------------------------ */
/* A create ID is a UUID: the SDK's CharInfoRaw::Verify (and so CharInfoAccessor::IsValid, which Mario Kart 8 builds a head model from) wants version 4 and the RFC 4122 variant bits. */
static void uuid_v4(uint8_t id[16]) {
    id[6] = (uint8_t)((id[6] & 0x0f) | 0x40);
    id[8] = (uint8_t)((id[8] & 0x3f) | 0x80);
}
static void set_id(MiiCharInfo *m, uint8_t tag, uint32_t index) {
    memset(m->create_id, 0, 16);
    memcpy(m->create_id, "NappleMii", 9);
    m->create_id[9] = tag;
    m->create_id[12] = (uint8_t)index;
    m->create_id[15] = 1;
    uuid_v4(m->create_id);
}
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { /* xorshift64*: a stream seeded per process; the pid is mixed in on every draw so a forked process never repeats its parent's ids */
    static int last_pid;
    if (last_pid != getpid()) {
        last_pid = getpid();
        rng_state ^= (uint64_t)last_pid * 0x9e3779b97f4a7c15ull;
    }
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static void seed_rng_once(void) {
    static int seeded;
    if (seeded) return;
    seeded = 1;
    rng_state ^= (uint64_t)(uintptr_t)&rng_state * 0x100000001b3ull ^ (uint64_t)getpid() << 20;
    for (int i = 0; i < 4; i++) rnd();
}
void mii_new_create_id(uint8_t id[16]) {
    seed_rng_once();
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = rnd();
        memcpy(id + i, &r, 4);
    }
    uuid_v4(id);
}

/* ---- the default Miis -------------------------------------------------------------------------------------------------------------- */
/* The six default Miis of the console, read from the Mii service's own data in the firmware (system program 0100000000000039 "sdb", 22.1.0, read-only data at
 * +0xa8a24: 54 u32 per entry, in this order; the service converts the 3DS-style colour indices through the tables below when it builds a CharInfo).
 * The entry order is the DefaultMii index games pass to BuildDefault; every entry carries the nickname "no name". */
typedef struct {
    uint32_t faceline_type, faceline_color, faceline_wrinkle, hair_type, hair_color, hair_flip;
    uint32_t eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    uint32_t eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    uint32_t nose_type, nose_scale, nose_y;
    uint32_t mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    uint32_t beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    uint32_t glasses_type, glasses_color, glasses_scale, glasses_y;
    uint32_t mole_type, mole_scale, mole_x, mole_y;
    uint32_t height, build, gender, favorite_color, pad[3];
} DefaultMii;
static const DefaultMii default_miis[MII_DEFAULT_COUNT] = {
    {4, 0, 0, 68, 0, 0, 2, 0, 4, 3, 4, 2, 12, 6, 0, 4, 3, 6, 2, 10, 1, 4, 9, 23, 0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 0, 4, {0}},
    {0, 0, 0, 55, 6, 0, 2, 4, 4, 3, 4, 2, 12, 6, 6, 4, 3, 6, 2, 10, 1, 4, 9, 23, 0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 0, 5, {0}},
    {1, 0, 0, 33, 1, 0, 2, 0, 4, 3, 4, 2, 12, 6, 1, 4, 3, 6, 2, 10, 1, 4, 9, 23, 0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 0, 0, {0}},
    {2, 0, 0, 24, 0, 0, 4, 0, 4, 3, 3, 2, 12, 0, 0, 4, 3, 6, 2, 10, 1, 4, 9, 23, 0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 1, 2, {0}},
    {0, 0, 0, 14, 7, 0, 4, 5, 4, 3, 3, 2, 12, 0, 7, 4, 3, 6, 2, 10, 1, 4, 9, 23, 0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 1, 6, {0}},
    {0, 0, 0, 12, 1, 0, 4, 0, 4, 3, 3, 2, 12, 0, 1, 4, 3, 6, 2, 10, 1, 4, 9, 23, 0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 1, 7, {0}},
};
/* 3DS colour index -> common colour index, as the service's own tables (sdb +0xa8756: faceline 0..5, hair 8,1..7, eye 8..13, mouth 19..23, glasses 8,14..18). */
static const uint8_t color_hair[8] = {8, 1, 2, 3, 4, 5, 6, 7}, color_eye[6] = {8, 9, 10, 11, 12, 13}, color_mouth[5] = {19, 20, 21, 22, 23}, color_glasses[7] = {8, 14, 15, 16, 17, 18, 0};
#define TAB(t, v) ((t)[(v) < sizeof(t) ? (v) : 0])

void mii_build_default(uint32_t index, MiiCharInfo *out) {
    const DefaultMii *d = &default_miis[index % MII_DEFAULT_COUNT];
    memset(out, 0, sizeof *out);
    set_id(out, 'D', index % MII_DEFAULT_COUNT);
    mii_utf8_to_utf16("no name", out->name, 10);
    out->favorite_color = (uint8_t)d->favorite_color;
    out->gender = (uint8_t)d->gender;
    out->height = (uint8_t)d->height;
    out->build = (uint8_t)d->build;
    out->faceline_type = (uint8_t)d->faceline_type;
    out->faceline_color = (uint8_t)d->faceline_color;
    out->faceline_wrinkle = (uint8_t)d->faceline_wrinkle;
    out->hair_type = (uint8_t)d->hair_type;
    out->hair_color = TAB(color_hair, d->hair_color);
    out->hair_flip = (uint8_t)d->hair_flip;
    out->eye_type = (uint8_t)d->eye_type;
    out->eye_color = TAB(color_eye, d->eye_color);
    out->eye_scale = (uint8_t)d->eye_scale;
    out->eye_aspect = (uint8_t)d->eye_aspect;
    out->eye_rotate = (uint8_t)d->eye_rotate;
    out->eye_x = (uint8_t)d->eye_x;
    out->eye_y = (uint8_t)d->eye_y;
    out->eyebrow_type = (uint8_t)d->eyebrow_type;
    out->eyebrow_color = TAB(color_hair, d->eyebrow_color);
    out->eyebrow_scale = (uint8_t)d->eyebrow_scale;
    out->eyebrow_aspect = (uint8_t)d->eyebrow_aspect;
    out->eyebrow_rotate = (uint8_t)d->eyebrow_rotate;
    out->eyebrow_x = (uint8_t)d->eyebrow_x;
    out->eyebrow_y = (uint8_t)d->eyebrow_y;
    out->nose_type = (uint8_t)d->nose_type;
    out->nose_scale = (uint8_t)d->nose_scale;
    out->nose_y = (uint8_t)d->nose_y;
    out->mouth_type = (uint8_t)d->mouth_type;
    out->mouth_color = TAB(color_mouth, d->mouth_color);
    out->mouth_scale = (uint8_t)d->mouth_scale;
    out->mouth_aspect = (uint8_t)d->mouth_aspect;
    out->mouth_y = (uint8_t)d->mouth_y;
    out->beard_color = TAB(color_hair, d->beard_color);
    out->beard_type = (uint8_t)d->beard_type;
    out->mustache_type = (uint8_t)d->mustache_type;
    out->mustache_scale = (uint8_t)d->mustache_scale;
    out->mustache_y = (uint8_t)d->mustache_y;
    out->glasses_type = (uint8_t)d->glasses_type;
    out->glasses_color = TAB(color_glasses, d->glasses_color);
    out->glasses_scale = (uint8_t)d->glasses_scale;
    out->glasses_y = (uint8_t)d->glasses_y;
    out->mole_type = (uint8_t)d->mole_type;
    out->mole_scale = (uint8_t)d->mole_scale;
    out->mole_x = (uint8_t)d->mole_x;
    out->mole_y = (uint8_t)d->mole_y;
}

/* BuildRandom: the firmware draws parts from per-gender, per-age and per-race tables that are not extracted yet; this draws every part from the full valid range of
 * its slider (hair and eye families by gender), always a valid Mii with a fresh create id. Age and race are accepted and ignored. */
void mii_build_random(uint32_t age, uint32_t gender, uint32_t race, MiiCharInfo *out) {
    (void)age;
    (void)race;
    seed_rng_once();
    uint32_t female = gender == MII_GENDER_FEMALE ? 1 : gender == MII_GENDER_MALE ? 0 : (rnd() & 1);
    mii_build_default(female ? 3 + rnd() % 3 : rnd() % 3, out);
    mii_new_create_id(out->create_id);
    out->gender = (uint8_t)female;
    out->favorite_color = (uint8_t)(rnd() % 12);
    out->height = (uint8_t)(40 + rnd() % 49);
    out->build = (uint8_t)(40 + rnd() % 49);
    out->faceline_type = (uint8_t)(rnd() % 12);
    out->faceline_color = (uint8_t)(rnd() % 6);
    out->hair_type = (uint8_t)(rnd() % 132);
    out->hair_color = TAB(color_hair, rnd() % 8);
    out->hair_flip = (uint8_t)(rnd() & 1);
    out->eye_type = (uint8_t)(rnd() % 60);
    out->eye_color = TAB(color_eye, rnd() % 6);
    out->eyebrow_type = (uint8_t)(rnd() % 24);
    out->eyebrow_color = out->hair_color;
    out->nose_type = (uint8_t)(rnd() % 18);
    out->mouth_type = (uint8_t)(rnd() % 36);
    out->mouth_color = TAB(color_mouth, rnd() % 5);
    out->beard_color = out->hair_color;
    out->beard_type = out->mustache_type = 0;
    if (!female && rnd() % 4 == 0) out->mustache_type = (uint8_t)(1 + rnd() % 5);
    out->glasses_type = rnd() % 5 == 0 ? (uint8_t)(1 + rnd() % 8) : 0;
}

/* ---- bit packing: CharInfo <-> CoreData ---------------------------------------------------------------------------------------------- */
/* CoreData is seven little-endian u32 words of bit fields followed by the nickname (10 UTF-16 units, no NUL). The field order inside the words is the author's
 * reconstruction of the public layout (every field is packed so that its slider range fits its width and each word is filled exactly or with padding bits);
 * it round-trips here, and what a game or the editor unpacks on its own side depends on it matching the console's. */
#define F(name, word, lsb, width) {offsetof(MiiCharInfo, name), word, lsb, width}
static const struct {
    uint8_t off, word, lsb, width;
} fields[] = {
    F(hair_type, 0, 0, 8),         F(height, 0, 8, 7),         F(mole_type, 0, 15, 1),      F(build, 0, 16, 7),          F(hair_flip, 0, 23, 1),        F(hair_color, 0, 24, 7),
    F(type, 0, 31, 1),             F(eye_color, 1, 0, 7),      F(gender, 1, 7, 1),          F(eyebrow_color, 1, 8, 7),   F(mouth_color, 1, 16, 7),      F(beard_color, 1, 24, 7),
    F(glasses_color, 2, 0, 7),     F(eye_type, 2, 8, 6),       F(region_move, 2, 14, 2),    F(mouth_type, 2, 16, 6),     F(font_region, 2, 22, 2),      F(eye_y, 2, 24, 5),
    F(glasses_scale, 2, 29, 3),    F(eyebrow_type, 3, 0, 5),   F(mustache_type, 3, 5, 3),   F(nose_type, 3, 8, 5),       F(beard_type, 3, 13, 3),       F(nose_y, 3, 16, 5),
    F(mouth_aspect, 3, 21, 3),     F(mouth_y, 3, 24, 5),       F(eyebrow_aspect, 3, 29, 3), F(mustache_y, 4, 0, 5),      F(eye_rotate, 4, 5, 3),        F(glasses_y, 4, 8, 5),
    F(eye_aspect, 4, 13, 3),       F(mole_x, 4, 16, 5),        F(eye_scale, 4, 21, 3),      F(mole_y, 4, 24, 5),         F(glasses_type, 5, 0, 4),      F(favorite_color, 5, 4, 4),
    F(faceline_type, 5, 8, 4),     F(faceline_color, 5, 12, 4), F(faceline_wrinkle, 5, 16, 4), F(faceline_make, 5, 20, 4), F(eye_x, 5, 24, 4),          F(eyebrow_scale, 5, 28, 4),
    F(eyebrow_rotate, 6, 0, 4),    F(eyebrow_x, 6, 4, 4),      F(eyebrow_y, 6, 8, 5),       F(nose_scale, 6, 13, 4),     F(mouth_scale, 6, 17, 4),      F(mustache_scale, 6, 21, 4),
    F(mole_scale, 6, 25, 4),
};
void mii_core_from_char_info(const MiiCharInfo *m, MiiCoreData *out) {
    uint32_t w[7] = {0};
    const uint8_t *b = (const uint8_t *)m;
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        uint32_t mask = (1u << fields[i].width) - 1;
        w[fields[i].word] |= ((uint32_t)b[fields[i].off] & mask) << fields[i].lsb;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->data, w, sizeof w);
    memcpy(out->data + 0x1c, m->name, 20); /* ten characters; the eleventh is the terminator */
}
void mii_char_info_from_core(const MiiCoreData *c, const uint8_t create_id[16], MiiCharInfo *out) {
    uint32_t w[7];
    memcpy(w, c->data, sizeof w);
    memset(out, 0, sizeof *out);
    uint8_t *b = (uint8_t *)out;
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) b[fields[i].off] = (uint8_t)((w[fields[i].word] >> fields[i].lsb) & ((1u << fields[i].width) - 1));
    memcpy(out->name, c->data + 0x1c, 20);
    out->name[10] = 0;
    if (create_id) memcpy(out->create_id, create_id, 16);
    else mii_new_create_id(out->create_id);
}

/* ---- checksums and StoreData ------------------------------------------------------------------------------------------------------------ */
uint16_t mii_crc16(const void *data, size_t n) { /* CRC-16/XMODEM: polynomial 0x1021, initial value 0, MSB first */
    const uint8_t *p = data;
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}
/* The firmware's mii library computes both StoreData checksums with its own routine (read from MiiEdit's main module, +0xec720..): the register starts at 0 (or at a seed),
 * each byte is shifted in at the bottom (8 shifts with polynomial 0x1021, then xor the byte) and the result is stored BIG-endian.
 *   data CRC   (offset 0x40): over bytes 0..0x3f followed by two zero bytes, which is CRC-16/XMODEM of those 0x40 bytes;
 *   device CRC (offset 0x42): the same routine over bytes 0..0x41 and two zero bytes, but starting from a seed = the routine run over the 16-byte author id (the
 *              `set:sys` GetMiiAuthorId value) without the trailing zero bytes. The editor refuses a Mii whose checksums do not match its own author id. */
static uint16_t crc_aug(uint16_t reg, const uint8_t *p, size_t n) {
    while (n--) {
        for (int i = 0; i < 8; i++) reg = (uint16_t)((reg & 0x8000) ? (reg << 1) ^ 0x1021 : reg << 1);
        reg ^= *p++;
    }
    return reg;
}
static uint8_t author_id[16] = {0x4e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x44, 0x65, 0x76, 0x69, 0x63, 0x65, 0, 0, 0, 1};
void mii_device_id(uint8_t out[16]) { memcpy(out, author_id, 16); }
static void seal(MiiStoreData *s) {
    uint8_t b[0x44];
    memcpy(b, s, 0x40);
    memset(b + 0x40, 0, 4);
    uint16_t dc = crc_aug(0, b, 0x42);
    b[0x40] = (uint8_t)(dc >> 8);
    b[0x41] = (uint8_t)dc;
    uint16_t seed = crc_aug(0, author_id, 16), cc = crc_aug(seed, b, 0x44);
    b[0x42] = (uint8_t)(cc >> 8);
    b[0x43] = (uint8_t)cc;
    memcpy(&s->data_crc, b + 0x40, 4);
}
void mii_store_from_char_info(const MiiCharInfo *m, MiiStoreData *out) {
    memset(out, 0, sizeof *out);
    mii_core_from_char_info(m, &out->core);
    memcpy(out->create_id, m->create_id, 16);
    seal(out);
}
void mii_char_info_from_store(const MiiStoreData *s, MiiCharInfo *out) { mii_char_info_from_core(&s->core, s->create_id, out); }
int mii_store_verify(const MiiStoreData *s) {
    MiiStoreData t = *s;
    seal(&t);
    if (t.data_crc != s->data_crc || t.device_crc != s->device_crc) return 0;
    MiiCharInfo c;
    mii_char_info_from_store(s, &c);
    return mii_char_info_valid(&c);
}

#define IN(v, lo, hi) ((v) >= (lo) && (v) <= (hi))
int mii_char_info_valid(const MiiCharInfo *m) {
    return IN(m->favorite_color, 0, 11) && m->gender <= 1 && m->height <= 127 && m->build <= 127 && IN(m->faceline_type, 0, 11) && IN(m->faceline_color, 0, 5) &&
           IN(m->faceline_wrinkle, 0, 11) && IN(m->faceline_make, 0, 11) && IN(m->hair_type, 0, 131) && IN(m->hair_color, 0, 99) && m->hair_flip <= 1 && IN(m->eye_type, 0, 59) &&
           IN(m->eye_color, 0, 99) && IN(m->eye_scale, 0, 7) && IN(m->eye_aspect, 0, 6) && IN(m->eye_rotate, 0, 7) && IN(m->eye_x, 0, 12) && IN(m->eye_y, 0, 18) &&
           IN(m->eyebrow_type, 0, 23) && IN(m->eyebrow_color, 0, 99) && IN(m->eyebrow_scale, 0, 8) && IN(m->eyebrow_aspect, 0, 6) && IN(m->eyebrow_rotate, 0, 11) &&
           IN(m->eyebrow_x, 0, 12) && IN(m->eyebrow_y, 3, 18) && IN(m->nose_type, 0, 17) && IN(m->nose_scale, 0, 8) && IN(m->nose_y, 0, 18) && IN(m->mouth_type, 0, 35) &&
           IN(m->mouth_color, 0, 99) && IN(m->mouth_scale, 0, 8) && IN(m->mouth_aspect, 0, 6) && IN(m->mouth_y, 0, 18) && IN(m->beard_color, 0, 99) && IN(m->beard_type, 0, 5) &&
           IN(m->mustache_type, 0, 5) && IN(m->mustache_scale, 0, 8) && IN(m->mustache_y, 0, 16) && IN(m->glasses_type, 0, 8) && IN(m->glasses_color, 0, 99) &&
           IN(m->glasses_scale, 0, 7) && IN(m->glasses_y, 0, 20) && IN(m->mole_type, 0, 1) && IN(m->mole_scale, 0, 8) && IN(m->mole_x, 0, 16) && IN(m->mole_y, 0, 30);
}

/* ---- the database ------------------------------------------------------------------------------------------------------------------------ */
/* One database for every title (each game is its own app, and on the console it is system save data 8000000000000030). It lives behind a StorageProvider so the
 * place can change per platform: the file provider below (macOS and Linux, <system dir>/mii/MiiDatabase.dat), later an iOS provider (App Group container, a folder
 * the user picked through a security-scoped bookmark, or import/export files). The provider supplies read / atomic write / lock / stamp (observe).
 * Every operation is lock -> refresh from storage if another process changed it -> mutate -> write -> unlock, so two games running at once never lose an edit.
 * File: 16-byte header {'N','F','D','B', u8 version 1, u8 count, u16 CRC-16 of the entries, u32 generation, u32 0} then count StoreData. The host's own container
 * (the console's save-data layout is not reproduced); what it stores is the console's StoreData. The previous copy is kept as <file>.bak; a missing or damaged
 * file falls back to the backup, then to the seed (the player's Mii), never an error to the game. */
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
static MiiStoreData db[MII_DB_MAX];
static uint32_t db_n, db_gen = 1;
static const MiiStorage *db_store;
static int db_loaded, db_broken;
static uint64_t db_stamp;

static void db_seed(void) {
    MiiCharInfo m;
    mii_build_default(2, &m); /* the neutral default Mii under the player's name, with an id of its own */
    memset(m.name, 0, sizeof m.name);
    mii_utf8_to_utf16(user_name, m.name, 10);
    uint8_t id[16] = {0};
    memcpy(id, "NappleUserMii", 13);
    uuid_v4(id);
    memcpy(m.create_id, id, 16);
    mii_store_from_char_info(&m, &db[0]);
    db_n = 1;
}
static int db_parse(const uint8_t *buf, size_t n, MiiStoreData *dst, uint32_t *count, uint32_t *gen) {
    if (n < 16 || memcmp(buf, "NFDB", 4) || buf[4] != 1 || buf[5] > MII_DB_MAX || n != 16 + (size_t)buf[5] * sizeof(MiiStoreData)) return 0;
    uint16_t crc;
    memcpy(&crc, buf + 6, 2);
    if (crc != mii_crc16(buf + 16, n - 16)) return 0;
    memcpy(dst, buf + 16, n - 16);
    for (unsigned i = 0; i < buf[5]; i++)
        if (!mii_store_verify(&dst[i])) return 0;
    *count = buf[5];
    if (gen) memcpy(gen, buf + 8, 4);
    return 1;
}
static size_t db_serialize(uint8_t *buf) {
    size_t body = (size_t)db_n * sizeof db[0];
    memset(buf, 0, 16);
    memcpy(buf, "NFDB", 4);
    buf[4] = 1;
    buf[5] = (uint8_t)db_n;
    uint16_t crc = mii_crc16(db, body);
    memcpy(buf + 6, &crc, 2);
    memcpy(buf + 8, &db_gen, 4);
    memcpy(buf + 16, db, body);
    return 16 + body;
}
static void db_save_locked(void) {
    if (!db_store) return;
    uint8_t buf[16 + MII_DB_MAX * sizeof(MiiStoreData)];
    size_t n = db_serialize(buf);
    if (db_store->write(db_store->ctx, buf, n) == 0 && db_store->stamp) db_stamp = db_store->stamp(db_store->ctx);
}
/* Brings the in-memory copy up to date with the storage (called with the lock held). */
static void db_refresh_locked(void) {
    if (!db_store) {
        if (!db_loaded) {
            db_loaded = 1;
            db_seed();
        }
        return;
    }
    uint64_t st = db_store->stamp ? db_store->stamp(db_store->ctx) : 0;
    if (db_loaded && st == db_stamp && st != 0) return;
    static uint8_t buf[16 + MII_DB_MAX * sizeof(MiiStoreData)];
    static MiiStoreData tmp[MII_DB_MAX];
    uint32_t count = 0, gen = 0;
    int found = 0;
    for (int which = 0; which < 2 && !found; which++) { /* the file, then the previous copy */
        size_t n = 0;
        int rc = db_store->read(db_store->ctx, which, buf, sizeof buf, &n);
        if (rc == 1) continue; /* absent */
        if (rc == 0 && db_parse(buf, n, tmp, &count, &gen)) {
            found = 1;
            if (which == 1) db_broken = 1;
        } else if (which == 0) db_broken = 1; /* present but damaged */
    }
    int first = !db_loaded;
    db_loaded = 1;
    db_stamp = st;
    if (found) {
        if (first || gen != db_gen || count != db_n || memcmp(tmp, db, (size_t)count * sizeof db[0])) {
            memcpy(db, tmp, (size_t)count * sizeof db[0]);
            db_n = count;
            db_gen = gen > db_gen || first ? gen : db_gen + 1;
        }
        return;
    }
    if (first || db_n == 0) {
        db_seed();
        db_gen++;
        db_save_locked();
    }
}
/* every public operation: lock, refresh, run */
static void begin(void) {
    pthread_mutex_lock(&db_lock);
    if (db_store && db_store->lock) db_store->lock(db_store->ctx);
    db_refresh_locked();
}
static void end(void) {
    if (db_store && db_store->unlock) db_store->unlock(db_store->ctx);
    pthread_mutex_unlock(&db_lock);
}

/* the file provider */
typedef struct {
    char path[1300], bak[1310], lock[1310];
    int lock_fd;
} FileStore;
static int fs_read(void *ctx, int which, uint8_t *buf, size_t cap, size_t *n) {
    FileStore *f = ctx;
    FILE *fp = fopen(which ? f->bak : f->path, "rb");
    if (!fp) return 1;
    *n = fread(buf, 1, cap, fp);
    int more = fgetc(fp) != EOF; /* larger than any valid database */
    fclose(fp);
    return more ? 2 : 0;
}
static int fs_write(void *ctx, const uint8_t *buf, size_t n) {
    FileStore *f = ctx;
    char tmp[1330];
    snprintf(tmp, sizeof tmp, "%s.tmp%d", f->path, (int)getpid());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return 2;
    int ok = fwrite(buf, 1, n, fp) == n && fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    ok = fclose(fp) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return 2;
    }
    rename(f->path, f->bak); /* keep the previous copy (a copy-by-rename: the file is replaced right after) */
    if (rename(tmp, f->path)) {
        rename(f->bak, f->path);
        remove(tmp);
        return 2;
    }
    return 0;
}
static void fs_lock(void *ctx) {
    FileStore *f = ctx;
    f->lock_fd = open(f->lock, O_RDWR | O_CREAT, 0644);
    if (f->lock_fd >= 0) flock(f->lock_fd, LOCK_EX);
}
static void fs_unlock(void *ctx) {
    FileStore *f = ctx;
    if (f->lock_fd >= 0) {
        flock(f->lock_fd, LOCK_UN);
        close(f->lock_fd);
        f->lock_fd = -1;
    }
}
static uint64_t fs_stamp(void *ctx) {
    FileStore *f = ctx;
    struct stat st;
    if (stat(f->path, &st)) return 0;
#ifdef __APPLE__
    uint64_t ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec;
#else
    uint64_t ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#endif
    return (ns ^ ((uint64_t)st.st_size << 52) ^ (uint64_t)st.st_ino) | 1;
}
static int fs_destroy(void *ctx) {
    FileStore *f = ctx;
    int a = remove(f->path), b = remove(f->bak);
    return a == 0 || b == 0 ? 0 : 1;
}
static FileStore file_store;
static MiiStorage file_provider = {&file_store, fs_read, fs_write, fs_lock, fs_unlock, fs_stamp, fs_destroy};

void mii_db_set_storage(const MiiStorage *s) {
    pthread_mutex_lock(&db_lock);
    db_store = s;
    db_loaded = 0;
    db_n = 0;
    db_gen++;
    pthread_mutex_unlock(&db_lock);
}
void mii_db_set_path(const char *path) { /* a new path (or none) makes the next use load from scratch: what a restart does */
    if (path) {
        snprintf(file_store.path, sizeof file_store.path, "%s", path);
        snprintf(file_store.bak, sizeof file_store.bak, "%s.bak", path);
        snprintf(file_store.lock, sizeof file_store.lock, "%s.lock", path);
        file_store.lock_fd = -1;
    }
    mii_db_set_storage(path ? &file_provider : NULL);
}
uint32_t mii_db_count(void) {
    begin();
    uint32_t n = db_n;
    end();
    return n;
}
uint32_t mii_db_generation(void) {
    begin();
    uint32_t g = db_gen;
    end();
    return g;
}
int mii_db_get(uint32_t index, MiiStoreData *out) {
    begin();
    int ok = index < db_n;
    if (ok) *out = db[index];
    end();
    return ok ? MII_OK : MII_ERR_NOT_FOUND;
}
static int find_locked(const uint8_t id[16]) {
    for (uint32_t i = 0; i < db_n; i++)
        if (!memcmp(db[i].create_id, id, 16)) return (int)i;
    return -1;
}
int mii_db_find(const uint8_t id[16]) {
    begin();
    int i = find_locked(id);
    end();
    return i;
}
int mii_db_add_or_replace(const MiiStoreData *s) {
    MiiStoreData t = *s;
    MiiCharInfo c;
    mii_char_info_from_store(&t, &c);
    if (!mii_char_info_valid(&c)) return MII_ERR_INVALID;
    seal(&t); /* the caller's CRCs are not trusted: the database stamps its own */
    begin();
    int i = find_locked(t.create_id), rc = MII_OK;
    if (i >= 0) db[i] = t;
    else if (db_n >= MII_DB_MAX) rc = MII_ERR_FULL;
    else db[db_n++] = t;
    if (rc == MII_OK) {
        db_gen++;
        db_save_locked();
    }
    end();
    return rc;
}
int mii_db_delete(const uint8_t id[16]) {
    begin();
    int i = find_locked(id), rc = MII_ERR_NOT_FOUND;
    if (i >= 0) {
        memmove(&db[i], &db[i + 1], (db_n - (uint32_t)i - 1) * sizeof db[0]);
        db_n--;
        db_gen++;
        db_save_locked();
        rc = MII_OK;
    }
    end();
    return rc;
}
int mii_db_move(const uint8_t id[16], uint32_t index) {
    begin();
    int i = find_locked(id), rc = MII_ERR_NOT_FOUND;
    if (i >= 0 && index < db_n) {
        MiiStoreData t = db[i];
        if ((uint32_t)i < index) memmove(&db[i], &db[i + 1], (index - (uint32_t)i) * sizeof db[0]);
        else memmove(&db[index + 1], &db[index], ((uint32_t)i - index) * sizeof db[0]);
        db[index] = t;
        db_gen++;
        db_save_locked();
        rc = MII_OK;
    } else if (i >= 0) rc = MII_ERR_INVALID;
    end();
    return rc;
}
void mii_db_format(void) {
    begin();
    db_n = 0;
    db_gen++;
    db_save_locked();
    end();
}
int mii_db_destroy_file(void) {
    begin();
    int rc = db_store && db_store->destroy && db_store->destroy(db_store->ctx) == 0 ? MII_OK : MII_ERR_NOT_FOUND;
    db_loaded = 0;
    db_n = 0;
    db_gen++;
    end();
    return rc;
}
int mii_db_export(uint8_t *buf, size_t cap, size_t *n) {
    begin();
    size_t need = 16 + (size_t)db_n * sizeof db[0];
    int rc = cap >= need ? MII_OK : MII_ERR_INVALID;
    if (rc == MII_OK) *n = db_serialize(buf);
    end();
    return rc;
}
int mii_db_import(const uint8_t *buf, size_t n) {
    static MiiStoreData tmp[MII_DB_MAX];
    uint32_t count;
    begin();
    int ok = db_parse(buf, n, tmp, &count, NULL);
    if (ok) {
        memcpy(db, tmp, (size_t)count * sizeof db[0]);
        db_n = count;
        db_gen++;
        db_save_locked();
    }
    end();
    return ok ? MII_OK : MII_ERR_INVALID;
}
int mii_db_take_broken(void) {
    begin();
    int b = db_broken;
    db_broken = 0;
    end();
    return b;
}

/* ---- the view games get: database Miis first, then the defaults ------------------------------------------------------------------------------ */
uint32_t mii_count(uint32_t f) { return ((f & MII_FLAG_DATABASE) ? mii_db_count() : 0u) + ((f & MII_FLAG_DEFAULT) ? MII_DEFAULT_COUNT : 0u); }

uint32_t mii_list(uint32_t f, MiiElement *out, uint32_t cap) {
    uint32_t n = 0;
    if (f & MII_FLAG_DATABASE) {
        uint32_t total = mii_db_count();
        for (uint32_t i = 0; i < total && n < cap; i++) {
            MiiStoreData s;
            if (mii_db_get(i, &s) != MII_OK) break;
            mii_char_info_from_store(&s, &out[n].info);
            out[n].source = MII_SOURCE_DATABASE;
            n++;
        }
    }
    if (f & MII_FLAG_DEFAULT)
        for (uint32_t i = 0; i < MII_DEFAULT_COUNT && n < cap; i++) {
            mii_build_default(i, &out[n].info);
            out[n].source = MII_SOURCE_DEFAULT;
            n++;
        }
    return n;
}

int mii_find_index(const uint8_t id[16]) { return mii_db_find(id); }
