/* mii core: the Mii data model and the database the host presents (portable C, no host services).
 *  - CharInfo (0x58): the unpacked Mii games read through nn::mii::Database::Get*.
 *  - CoreData (0x30) / StoreData (0x44): the packed forms the database stores and the editor exchanges (bit layout in mii_core.c).
 *  - the six default Miis, read from the firmware's own mii service (sdb, system program 0100000000000039).
 *  - the database: up to 100 StoreData in a file (header 'NFDB' + CRC), persisted on every change.
 * Where a layout is the author's reconstruction rather than a firmware fact, mii_core.c says so. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t create_id[16];
    uint16_t name[11]; /* UTF-16, NUL terminated */
    uint8_t font_region, favorite_color, gender, height, build, type, region_move;
    uint8_t faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    uint8_t hair_type, hair_color, hair_flip;
    uint8_t eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    uint8_t eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    uint8_t nose_type, nose_scale, nose_y;
    uint8_t mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    uint8_t beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    uint8_t glasses_type, glasses_color, glasses_scale, glasses_y;
    uint8_t mole_type, mole_scale, mole_x, mole_y;
    uint8_t padding;
} MiiCharInfo;
typedef struct {
    MiiCharInfo info;
    uint32_t source;
} MiiElement; /* 0x5c: CharInfo + where it came from */
typedef struct {
    uint8_t data[0x30]; /* 7 bit-field words (0x1c bytes) + the 10-character nickname (0x14 bytes) */
} MiiCoreData;
typedef struct {
    MiiCoreData core;
    uint8_t create_id[16];
    uint16_t data_crc;   /* stored big-endian (so the two bytes read as the CRC): CRC-16/XMODEM of the first 0x40 bytes */
    uint16_t device_crc; /* big-endian: the same routine over the first 0x42 bytes, seeded from the author id (see mii_core.c) */
} MiiStoreData;
typedef struct {
    MiiStoreData store;
    uint32_t source;
} MiiStoreElement; /* 0x48 */
#pragma pack(pop)
_Static_assert(sizeof(MiiCharInfo) == 0x58, "CharInfo is 0x58 bytes");
_Static_assert(sizeof(MiiElement) == 0x5c, "CharInfo element is 0x5c bytes");
_Static_assert(sizeof(MiiCoreData) == 0x30, "CoreData is 0x30 bytes");
_Static_assert(sizeof(MiiStoreData) == 0x44, "StoreData is 0x44 bytes");
_Static_assert(sizeof(MiiStoreElement) == 0x48, "StoreData element is 0x48 bytes");

enum { MII_SOURCE_DATABASE = 0, MII_SOURCE_DEFAULT = 1 }; /* Source values */
enum { MII_FLAG_DATABASE = 1, MII_FLAG_DEFAULT = 2 };     /* SourceFlag bits */
enum { MII_DEFAULT_COUNT = 6 };
enum { MII_GENDER_MALE = 0, MII_GENDER_FEMALE = 1 };
enum { MII_DB_MAX = 100 };
enum { MII_OK = 0, MII_ERR_NOT_FOUND = 1, MII_ERR_FULL = 2, MII_ERR_INVALID = 3 };

void mii_set_user_name(const char *utf8);                                              // the player's Mii takes this name (UTF-8 BMP, max 10 characters)
void mii_build_default(uint32_t index, MiiCharInfo *out);                              // index wraps modulo MII_DEFAULT_COUNT
void mii_build_random(uint32_t age, uint32_t gender, uint32_t race, MiiCharInfo *out); // a valid random Mii of the requested gender (0 male, 1 female, else either)
uint32_t mii_count(uint32_t source_flags);
uint32_t mii_list(uint32_t source_flags, MiiElement *out, uint32_t capacity); // returns the number written; database entries first
int mii_find_index(const uint8_t create_id[16]);                              // index in the database, -1 if absent
int mii_char_info_valid(const MiiCharInfo *m);                                // every field inside the range the editor can produce
size_t mii_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);           // BMP only; returns code units written (without the NUL)

/* conversions and checksums */
uint16_t mii_crc16(const void *data, size_t n);
void mii_device_id(uint8_t out[16]); // the Mii author id: what set:sys GetMiiAuthorId returns and the StoreData device CRC is seeded with
void mii_core_from_char_info(const MiiCharInfo *m, MiiCoreData *out);
void mii_char_info_from_core(const MiiCoreData *c, const uint8_t create_id[16], MiiCharInfo *out); // create_id NULL: a fresh one
void mii_store_from_char_info(const MiiCharInfo *m, MiiStoreData *out);                           // seals the CRCs
void mii_char_info_from_store(const MiiStoreData *s, MiiCharInfo *out);
int mii_store_verify(const MiiStoreData *s); // CRCs and field ranges
void mii_new_create_id(uint8_t id[16]);      // a fresh UUID (version 4, RFC 4122 variant), what the editor stamps on a new Mii

/* Where the database lives. read: which 0 = the file, 1 = the previous copy; returns 0 ok, 1 absent, 2 unreadable. write: atomic replace that keeps the previous copy.
 * lock/unlock: exclusive across processes. stamp: changes whenever another process rewrote the storage (0: unknown). destroy: removes it. */
typedef struct MiiStorage {
    void *ctx;
    int (*read)(void *ctx, int which, uint8_t *buf, size_t cap, size_t *n);
    int (*write)(void *ctx, const uint8_t *buf, size_t n);
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    uint64_t (*stamp)(void *ctx);
    int (*destroy)(void *ctx);
} MiiStorage;
void mii_db_set_storage(const MiiStorage *s); // NULL: memory only (seeded each run); the iOS providers plug in here

/* the database (one per process; the file is the console's shared Mii database) */
void mii_db_set_path(const char *path);      // file the database lives in (NULL: memory only); loaded on first use, rewritten on every change
uint32_t mii_db_count(void);
uint32_t mii_db_generation(void);            // bumped by every change: what IsUpdated compares
int mii_db_get(uint32_t index, MiiStoreData *out);
int mii_db_find(const uint8_t create_id[16]);
int mii_db_add_or_replace(const MiiStoreData *s); // replaces the entry with the same create id, else appends
int mii_db_delete(const uint8_t create_id[16]);
int mii_db_move(const uint8_t create_id[16], uint32_t index);
void mii_db_format(void);                    // empties the database (the player's Mii is not re-created)
int mii_db_destroy_file(void);               // removes the file; the next use starts from the seeded database again
int mii_db_export(uint8_t *buf, size_t cap, size_t *n);
int mii_db_import(const uint8_t *buf, size_t n);
int mii_db_take_broken(void);                // 1 once if the file on disk was damaged and was rebuilt (clears the flag)
