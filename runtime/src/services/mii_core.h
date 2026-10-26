/* mii core: the Mii database the host presents (portable). One Mii for the local player plus the six built-in default Miis.
 * Layout of CharInfo (0x58 bytes) follows the public description of nn::mii::CharInfo as remembered by the author; it is checked only
 * for size and value ranges here, not against a real console or title. */
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
#pragma pack(pop)
_Static_assert(sizeof(MiiCharInfo) == 0x58, "CharInfo is 0x58 bytes");
_Static_assert(sizeof(MiiElement) == 0x5c, "CharInfo element is 0x5c bytes");

enum { MII_SOURCE_DATABASE = 0, MII_SOURCE_DEFAULT = 1 }; /* Source values */
enum { MII_FLAG_DATABASE = 1, MII_FLAG_DEFAULT = 2 };     /* SourceFlag bits */
enum { MII_DEFAULT_COUNT = 6 };
enum { MII_GENDER_MALE = 0, MII_GENDER_FEMALE = 1 };

void mii_set_user_name(const char *utf8);                   // the player's Mii takes this name (ASCII/UTF-8 BMP, max 10 characters)
void mii_build_default(uint32_t index, MiiCharInfo *out);   // index wraps modulo MII_DEFAULT_COUNT
void mii_build_random(uint32_t age, uint32_t gender, uint32_t race, MiiCharInfo *out);   // deterministic stand-in: a default Mii of the requested gender
uint32_t mii_count(uint32_t source_flags);
uint32_t mii_list(uint32_t source_flags, MiiElement *out, uint32_t capacity);   // returns the number written; database entries first
int mii_find_index(const uint8_t create_id[16]);                                // index in the database, -1 if absent
int mii_char_info_valid(const MiiCharInfo *m);                                  // every field inside the range the editor can produce
size_t mii_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);             // BMP only; returns code units written (without the NUL)
