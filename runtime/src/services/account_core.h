/* Account profiles (portable): the list of local users acc:u0 reports and player select offers. The first profile is the fixed local user (g_user_id);
 * NAPPLE_PROFILES=<n> (2..8) adds n-1 more named "Player 2".. whose ids differ in the last byte. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define ACCOUNT_MAX_PROFILES 8u
/* Number of profiles for a NAPPLE_PROFILES value (NULL/empty/garbage = 1, clamped to 1..ACCOUNT_MAX_PROFILES). */
unsigned account_profile_count_spec(const char *spec);
/* The uid of profile `index` (base = the first profile's uid); profile 0 is `base` unchanged. */
void account_profile_uid(const uint8_t base[16], unsigned index, uint8_t out[16]);
/* Index of `uid` among `count` profiles, or -1. */
int account_profile_find(const uint8_t base[16], unsigned count, const uint8_t uid[16]);
/* Display name: profile 0 is `first_name`, the others "Player N". */
void account_profile_name(const char *first_name, unsigned index, char *out, size_t cap);
