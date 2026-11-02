/* friend:u local state (portable). See friend_core.h. */
#include "friend_core.h"
#include "../sha256.h"
#include <stdio.h>
#include <string.h>

static const uint8_t zero16[16];

FriendUser *friend_user(FriendStore *st, const uint8_t user[16]) {
    if (!user || !memcmp(user, zero16, 16)) return NULL;
    FriendUser *fr = NULL;
    for (int i = 0; i < FRIEND_USERS_MAX; i++) {
        if (st->users[i].used && !memcmp(st->users[i].user, user, 16)) return &st->users[i];
        if (!st->users[i].used && !fr) fr = &st->users[i];
    }
    if (!fr) return NULL;
    memset(fr, 0, sizeof *fr);
    fr->used = 1;
    memcpy(fr->user, user, 16);
    /* Console defaults of a new user [unverified]: presence shown to friends (2), play log shown to friends (2), requests from anyone (1). */
    fr->presence_permission = 2, fr->play_log_permission = 2, fr->friend_request_reception = 1;
    return fr;
}

int friend_key_valid(const uint8_t key[0x40]) { return key && key[0] == 0x01 && key[1] == 0x01 && memcmp(key + 0x10, zero16, 16) != 0; }

static int key_check(const FriendUser *u, const uint8_t key[0x40]) {
    if (!friend_key_valid(key)) return -1;
    if (!memcmp(key + 0x10, u->user, 16)) return -2;
    return 0;
}
static void copy_name(uint8_t dst[FRIEND_SCREEN_NAME_SIZE], const uint8_t *src) {
    memset(dst, 0, FRIEND_SCREEN_NAME_SIZE);
    if (src) memcpy(dst, src, FRIEND_SCREEN_NAME_SIZE);
    dst[0x3f] = 0; /* the name is NUL-terminated within its 0x40 bytes */
}

int friend_history_add(FriendUser *u, const uint8_t key[0x40], const uint8_t *name, const uint8_t *my_name, uint64_t application_id, int64_t now) {
    int r = key_check(u, key);
    if (r) return r;
    if (u->nhistory == FRIEND_HISTORY_MAX) memmove(u->history, u->history + 1, sizeof u->history[0] * (FRIEND_HISTORY_MAX - 1)), u->nhistory--;
    FriendHistory *h = &u->history[u->nhistory++];
    memcpy(h->other, key + 0x10, 16);
    copy_name(h->name, name);
    copy_name(h->my_name, my_name);
    h->application_id = application_id;
    h->time = now;
    h->local = key[4] != 0;
    return 0;
}

void friend_history_stats(const FriendUser *u, uint64_t out[3]) {
    out[0] = u->nhistory, out[1] = out[2] = 0;
    for (uint32_t i = 0; i < u->nhistory; i++) out[u->history[i].local ? 1 : 2]++;
}

/* Record [unverified]: other user id @0, InAppScreenName name @0x10, my name @0x58, u64 application id @0xa0, s64 posix time @0xa8, u8 local @0xb0. */
static void encode_rec(uint8_t *o, const uint8_t other[16], const uint8_t *name, const uint8_t *my_name, uint64_t app, int64_t t, uint8_t local) {
    memset(o, 0, FRIEND_HISTORY_RECORD_SIZE);
    memcpy(o, other, 16);
    memcpy(o + 0x10, name, FRIEND_SCREEN_NAME_SIZE);
    memcpy(o + 0x58, my_name, FRIEND_SCREEN_NAME_SIZE);
    memcpy(o + 0xa0, &app, 8);
    memcpy(o + 0xa8, &t, 8);
    o[0xb0] = local;
}

uint32_t friend_history_encode(const FriendUser *u, uint32_t offset, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = offset; i < u->nhistory && n < max; i++, n++) {
        const FriendHistory *h = &u->history[u->nhistory - 1 - i];
        encode_rec(out + (size_t)n * FRIEND_HISTORY_RECORD_SIZE, h->other, h->name, h->my_name, h->application_id, h->time, h->local);
    }
    return n;
}

uint32_t friend_candidates_encode(const FriendUser *u, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < u->nhistory && n < max; i++) {
        const FriendHistory *h = &u->history[u->nhistory - 1 - i];
        int seen = 0;
        for (uint32_t j = 0; j < n && !seen; j++) seen = !memcmp(out + (size_t)j * FRIEND_HISTORY_RECORD_SIZE, h->other, 16);
        if (seen) continue;
        encode_rec(out + (size_t)n++ * FRIEND_HISTORY_RECORD_SIZE, h->other, h->name, h->my_name, h->application_id, h->time, h->local);
    }
    return n;
}

int friend_faced_pending(const FriendUser *u, const uint8_t other[16]) {
    for (uint32_t i = 0; i < u->nfaced; i++)
        if (!memcmp(u->faced[i].other, other, 16)) return 1;
    return 0;
}

int friend_faced_add(FriendUser *u, const uint8_t key[0x40], const uint8_t *name, const uint8_t *my_name, uint64_t application_id, int64_t now) {
    int r = key_check(u, key);
    if (r) return r;
    if (friend_faced_pending(u, key + 0x10)) return -3;
    if (u->nfaced == FRIEND_FACED_MAX) memmove(u->faced, u->faced + 1, sizeof u->faced[0] * (FRIEND_FACED_MAX - 1)), u->nfaced--;
    FriendFaced *f = &u->faced[u->nfaced++];
    memcpy(f->other, key + 0x10, 16);
    copy_name(f->name, name);
    copy_name(f->my_name, my_name);
    f->application_id = application_id;
    f->time = now;
    return 0;
}

uint32_t friend_faced_encode(const FriendUser *u, uint32_t offset, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = offset; i < u->nfaced && n < max; i++, n++) {
        const FriendFaced *f = &u->faced[u->nfaced - 1 - i];
        encode_rec(out + (size_t)n * FRIEND_FACED_RECORD_SIZE, f->other, f->name, f->my_name, f->application_id, f->time, 1);
    }
    return n;
}

void friend_presence_set(FriendUser *u, const void *p, size_t n) {
    if (n > FRIEND_PRESENCE_MAX) n = FRIEND_PRESENCE_MAX;
    memset(u->presence, 0, sizeof u->presence);
    if (p && n) memcpy(u->presence, p, n);
    u->presence_len = (uint32_t)n;
}

void friend_code(const uint8_t inst[16], const uint8_t user[16], char *out, size_t n) {
    uint8_t in[32 + 6], h[32];
    memcpy(in, inst, 16);
    memcpy(in + 16, user, 16);
    memcpy(in + 32, "friend", 6);
    sha256(in, sizeof in, h);
    uint64_t v = 0;
    memcpy(&v, h, 8);
    v %= 1000000000000ull;
    snprintf(out, n, "SW-%04u-%04u-%04u", (unsigned)(v / 100000000ull), (unsigned)(v / 10000ull % 10000ull), (unsigned)(v % 10000ull));
}

void friend_user_setting_encode(const FriendUser *u, const uint8_t inst[16], uint8_t out[FRIEND_USER_SETTING_SIZE]) {
    memset(out, 0, FRIEND_USER_SETTING_SIZE);
    memcpy(out, u->user, 16);
    memcpy(out + 0x10, &u->presence_permission, 4);
    memcpy(out + 0x14, &u->play_log_permission, 4);
    memcpy(out + 0x18, &u->friend_request_reception, 8);
    friend_code(inst, u->user, (char *)out + 0x20, 0x20);
    /* friend_code_next_issuable_time @0x40 stays 0: a new code can be issued at any time (no code was ever reissued). */
}
