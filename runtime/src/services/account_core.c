#include "account_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned account_profile_count_spec(const char *spec) {
    long n = spec && *spec ? strtol(spec, NULL, 10) : 1;
    if (n < 1) n = 1;
    if (n > (long)ACCOUNT_MAX_PROFILES) n = (long)ACCOUNT_MAX_PROFILES;
    return (unsigned)n;
}
void account_profile_uid(const uint8_t base[16], unsigned index, uint8_t out[16]) {
    memcpy(out, base, 16);
    out[15] = (uint8_t)(base[15] + index); /* base ends in 1: the extra users end in 2, 3, ... */
}
int account_profile_find(const uint8_t base[16], unsigned count, const uint8_t uid[16]) {
    uint8_t u[16];
    for (unsigned i = 0; i < count && i < ACCOUNT_MAX_PROFILES; i++) {
        account_profile_uid(base, i, u);
        if (!memcmp(u, uid, 16)) return (int)i;
    }
    return -1;
}
void account_profile_name(const char *first_name, unsigned index, char *out, size_t cap) {
    if (!cap) return;
    if (index == 0) snprintf(out, cap, "%s", first_name ? first_name : "");
    else snprintf(out, cap, "Player %u", index + 1);
}
