/* amiibo core: the virtual amiibo model, its on-disk JSON form, and the optional dump (NTAG215 image) import and export (portable).
 *
 * Primary design (like emuiibo): an amiibo is its 8-byte model id plus a generated UID, a nickname and owner, a register date and a locally persisted
 * application area. No dump and no key are needed; games only ever see what the nfp service returns. A virtual amiibo is one small JSON file in the amiibo
 * store (<save-root>/system/amiibo/<uid>.json), shared by every game, so a game's save in the application area persists across runs and titles.
 *
 * Real dumps are optional: with the user's retail key file (key_retail.bin, 160 bytes: 80 data-key bytes then 80 tag-key bytes, supplied like prod.keys,
 * never shipped) a 540-byte NTAG215 dump decrypts into a virtual amiibo, and a virtual amiibo packs back into a dump. The scheme (HMAC-SHA256 key derivation,
 * AES-128-CTR, two HMAC signatures) is the publicly documented amiibo format as the author remembers it; it is exercised only with a test key that is
 * generated in the tests, so compatibility with real dumps is UNVERIFIED. This repository contains no keys and no amiibo dumps. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AMIIBO_TAG_SIZE 540      /* NTAG215 user memory image */
#define AMIIBO_APP_AREA_SIZE 216 /* 0xD8 */
#define AMIIBO_NICK_BYTES 41     /* 10 characters of UTF-8 (up to 4 bytes each) would be 40; 40 + NUL */
#define AMIIBO_MII_CHARINFO 0x58

typedef struct {
    uint16_t year; /* full year, e.g. 2026 */
    uint8_t month, day;
} AmiiboDate;

typedef struct {
    uint8_t id[8]; /* model id: game/character (2), variant (1), type (1: 0 figure, 1 card, 2 yarn), model number (2), series (1), 0x02 */
    uint8_t uid[7];
    char nickname[AMIIBO_NICK_BYTES];       /* UTF-8, at most 10 characters */
    uint8_t owner_mii[AMIIBO_MII_CHARINFO]; /* nn::mii::CharInfo of the owner, opaque here */
    uint8_t owner_raw[0x60];                /* the 3DS-format Mii of an imported dump, kept so an export round-trips; opaque */
    int has_owner_raw;
    uint8_t registered; /* the amiibo was set up (nickname and owner); RegisterInfo is only valid then */
    AmiiboDate register_date, write_date;
    uint16_t write_counter;
    uint8_t version;
    uint8_t font_region;
    uint8_t has_app_area;
    uint32_t app_access_id;
    uint64_t app_id; /* title id that created the area */
    uint16_t app_write_counter;
    uint8_t app_data[AMIIBO_APP_AREA_SIZE];
} Amiibo;

/* Fresh, unregistered amiibo for a model id with a generated UID (first byte 0x04, NXP). `seed` makes the UID deterministic (tests); 0 draws random bytes. */
void amiibo_new(Amiibo *a, const uint8_t id[8], uint64_t seed);
int amiibo_id_from_hex(const char *hex16, uint8_t id[8]); /* "01010000000400 02" style, 16 hex digits, optional 0x prefix; 0 or -1 */
void amiibo_id_to_hex(const uint8_t id[8], char out[17]);
void amiibo_uid_to_hex(const uint8_t uid[7], char out[15]);
/* Resets to the unregistered, empty-area state (nfp Format). Keeps the id and UID. */
void amiibo_format(Amiibo *a);
void amiibo_set_register(Amiibo *a, const char *nickname_utf8, const uint8_t mii[AMIIBO_MII_CHARINFO], AmiiboDate when);
/* Application area: create fails (-1) when one exists; size must be <= 216 (-2). The rest of the area is filled with `fill` bytes (random for a real console). */
int amiibo_area_create(Amiibo *a, uint32_t access_id, uint64_t app_id, const void *data, size_t n, uint8_t fill);
void amiibo_area_delete(Amiibo *a);

/* ---- JSON form ---------------------------------------------------------------------------------------------------------------- */
/* Writes the JSON document; returns its length, or 0 when cap is too small. */
size_t amiibo_to_json(const Amiibo *a, char *out, size_t cap);
/* Parses a document written by amiibo_to_json (or by hand: only "id" is required). Returns 0, or -1 for malformed input, -2 for a missing or bad id. */
int amiibo_from_json(const char *text, size_t n, Amiibo *a);
/* The store: one <uid>.json per amiibo in `dir`. Writes go to a temporary file first and are renamed into place (a crash never leaves half a file). 0 or -1. */
int amiibo_store_save(const char *dir, const Amiibo *a);
int amiibo_store_load(const char *dir, const uint8_t uid[7], Amiibo *a); /* 0, -1 missing, -2 malformed */
/* Finds the first stored amiibo with this model id (the lowest UID file name). 0, or -1 when there is none. */
int amiibo_store_find_by_id(const char *dir, const uint8_t id[8], Amiibo *a);
/* Lists the stored amiibo UIDs; returns the count written (at most max). */
size_t amiibo_store_list(const char *dir, uint8_t (*uids)[7], size_t max);
int amiibo_store_delete(const char *dir, const uint8_t uid[7]);

int ami_hex_to_bytes(const char *hex, size_t ndigits, uint8_t *out); /* ndigits hex digits (even) to bytes; 0 or -1 */
/* base64 (standard alphabet, padded), exposed for the JSON form and tests. Encode returns the length; decode returns the byte count or -1. */
size_t ami_b64_encode(const void *in, size_t n, char *out, size_t cap);
long ami_b64_decode(const char *in, size_t n, void *out, size_t cap);

/* ---- dump import and export (optional; needs the user's key file) --------------------------------------------------------------- */
typedef struct {
    uint8_t data[80], tag[80]; /* master key sets: hmac key 16, type string 14, rfu 1, magic size 1, magic 16, xor pad 32 */
} AmiiboKeys;
/* Loads key_retail.bin (160 bytes). 0 ok; -1 file missing (errno stays set); -2 wrong size. */
int amiibo_keys_load(const char *path, AmiiboKeys *k);
/* NTAG215 image (hardware page order) -> decrypted internal layout, verifying both signatures. 0 ok, -1 signature mismatch (wrong key or a damaged dump). */
int amiibo_tag_unpack(const AmiiboKeys *k, const uint8_t tag[AMIIBO_TAG_SIZE], uint8_t plain[AMIIBO_TAG_SIZE]);
/* Internal layout -> signed, encrypted NTAG215 image. */
void amiibo_tag_pack(const AmiiboKeys *k, const uint8_t plain[AMIIBO_TAG_SIZE], uint8_t tag[AMIIBO_TAG_SIZE]);
void amiibo_to_plain(const Amiibo *a, uint8_t plain[AMIIBO_TAG_SIZE]);
int amiibo_from_plain(const uint8_t plain[AMIIBO_TAG_SIZE], Amiibo *a);
/* The whole import: tag image + keys -> virtual amiibo. 0, -1 bad signature, -2 not an amiibo layout. */
int amiibo_import_dump(const AmiiboKeys *k, const uint8_t tag[AMIIBO_TAG_SIZE], Amiibo *a);
void amiibo_export_dump(const AmiiboKeys *k, const Amiibo *a, uint8_t tag[AMIIBO_TAG_SIZE]);
/* Hex of the exact file a missing-key message must name. */
#define AMIIBO_KEY_FILE_NAME "key_retail.bin"

/* ---- primitives (exposed for tests) --------------------------------------------------------------------------------------------- */
void ami_sha256(const void *in, size_t n, uint8_t out[32]);
void ami_hmac_sha256(const uint8_t *key, size_t keylen, const void *in, size_t n, uint8_t out[32]);
void ami_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void ami_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n);

/* ---- small JSON reader shared with the library loader -------------------------------------------------------------------------- */
typedef struct {
    const char *p;
    size_t n;
    char kind; /* 's' string (span excludes the quotes), 'n' number/bool/null, 'o' object, 'a' array */
} AmiJVal;
const char *ami_jws(const char *p, const char *e);                       /* skips white space */
const char *ami_jstring_end(const char *p, const char *e);               /* p at an opening quote; returns past the closing quote or NULL */
const char *ami_jskip(const char *p, const char *e, AmiJVal *v);         /* skips one value (v optional); returns past it or NULL */
int ami_jget(const char *text, size_t n, const char *key, AmiJVal *out); /* top-level member of an object: 0 found, 1 absent, -1 malformed */
long ami_jstr(const AmiJVal *v, char *out, size_t cap);                  /* decodes a string span to UTF-8; length or -1 */
/* Iterates the members of an object: *cur starts at the object text; each call yields the next key (string span) and value. 1 = got one, 0 = end, -1 = malformed. */
int ami_jiter(const char **cur, const char *end, AmiJVal *key, AmiJVal *val);
