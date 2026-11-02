/* friend:u local state (portable): what the friends service keeps on the console itself, per local user, whether or not a provider serves the
 * friend list — the play history (AddPlayHistory, read back by GetPlayHistoryList / Statistics / GetFriendCandidateList), the in-person ("faced")
 * friend requests queued until the next sync, the user's own presence as the title last set it (GetUserPresenceView) and the user setting
 * (LoadUserSetting: libnx FriendsUserSetting layout, friend code derived from the installation id). Records the public docs do not describe are
 * Napple's own encodings, marked [unverified] where they are encoded for the guest. Nothing is persisted: the store lives for the process. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define FRIEND_USERS_MAX 8
#define FRIEND_HISTORY_MAX 64
#define FRIEND_FACED_MAX 32
#define FRIEND_PRESENCE_MAX 0xC0
#define FRIEND_SCREEN_NAME_SIZE 0x48 /* nn::friends::InAppScreenName {char name[0x40]; u64 language code} (switchbrew, libnx) */
#define FRIEND_USER_SETTING_SIZE 0x800 /* libnx FriendsUserSetting */
#define FRIEND_HISTORY_RECORD_SIZE 0x100 /* [unverified] Napple encoding of a play history entry */
#define FRIEND_FACED_RECORD_SIZE 0x100   /* [unverified] Napple encoding of a pending (sent) friend request */

typedef struct {
    uint8_t other[16]; /* the other player's user id, from their registration key */
    uint8_t name[FRIEND_SCREEN_NAME_SIZE], my_name[FRIEND_SCREEN_NAME_SIZE];
    uint64_t application_id;
    int64_t time;
    uint8_t local; /* key made for a local (wireless) session, else online */
} FriendHistory;

typedef struct {
    uint8_t other[16];
    uint8_t name[FRIEND_SCREEN_NAME_SIZE], my_name[FRIEND_SCREEN_NAME_SIZE];
    uint64_t application_id;
    int64_t time;
} FriendFaced;

typedef struct {
    uint8_t user[16], used;
    uint32_t presence_permission, play_log_permission;
    uint64_t friend_request_reception;
    FriendHistory history[FRIEND_HISTORY_MAX]; /* oldest first; full = the oldest is dropped */
    uint32_t nhistory;
    FriendFaced faced[FRIEND_FACED_MAX];
    uint32_t nfaced;
    uint8_t presence[FRIEND_PRESENCE_MAX];
    uint32_t presence_len;
} FriendUser;

typedef struct {
    FriendUser users[FRIEND_USERS_MAX];
} FriendStore;

/* The user's record, made with the console defaults on first use (NULL for a zero user id or a full table). */
FriendUser *friend_user(FriendStore *st, const uint8_t user[16]);
/* Play history registration keys (install_id_core.c layout): type 0x101 at 0, local flag at 4, user id at 0x10. 1 when `key` has that shape. */
int friend_key_valid(const uint8_t key[0x40]);
/* AddPlayHistory: 0 ok, -1 malformed key, -2 the key is the user's own (nobody plays with themself). */
int friend_history_add(FriendUser *u, const uint8_t key[0x40], const uint8_t *name, const uint8_t *my_name, uint64_t application_id, int64_t now);
/* {total, local, online} entries. */
void friend_history_stats(const FriendUser *u, uint64_t out[3]);
/* Encoders: write up to `max` records starting at `offset` (newest first) into out; return the number written. */
uint32_t friend_history_encode(const FriendUser *u, uint32_t offset, uint8_t *out, uint32_t max);
/* Distinct players of the play history, newest first (GetFriendCandidateList), same record encoding. */
uint32_t friend_candidates_encode(const FriendUser *u, uint8_t *out, uint32_t max);
/* AddFacedFriendRequestForApplication: 0 ok, -1 malformed key, -2 own key, -3 already pending for that player (kept once). */
int friend_faced_add(FriendUser *u, const uint8_t key[0x40], const uint8_t *name, const uint8_t *my_name, uint64_t application_id, int64_t now);
int friend_faced_pending(const FriendUser *u, const uint8_t other[16]);
uint32_t friend_faced_encode(const FriendUser *u, uint32_t offset, uint8_t *out, uint32_t max);
void friend_presence_set(FriendUser *u, const void *p, size_t n);
/* "SW-NNNN-NNNN-NNNN": 12 decimal digits from a hash of (installation, user), stable for both. out >= 18 bytes. */
void friend_code(const uint8_t inst[16], const uint8_t user[16], char *out, size_t n);
/* LoadUserSetting: FriendsUserSetting {uid @0, u32 presence_permission @0x10, u32 play_log_permission @0x14, u64 friend_request_reception @0x18,
 * char friend_code[0x20] @0x20, u64 friend_code_next_issuable_time @0x40, rest reserved} (libnx), FRIEND_USER_SETTING_SIZE bytes. */
void friend_user_setting_encode(const FriendUser *u, const uint8_t inst[16], uint8_t out[FRIEND_USER_SETTING_SIZE]);
