#include "bcat_core.h"
#include <string.h>

static int valid(const char *n, int allow_dot) {
    size_t len = strnlen(n, BCAT_NAME_SIZE);
    if (len == 0 || len >= BCAT_NAME_SIZE) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (allow_dot && c == '.'))) return 0;
    }
    for (size_t i = len; i < BCAT_NAME_SIZE; i++)
        if (n[i]) return 0; /* padding must be NUL */
    return 1;
}
int bcat_valid_directory_name(const char n[BCAT_NAME_SIZE]) { return valid(n, 0); }
int bcat_valid_file_name(const char n[BCAT_NAME_SIZE]) { return valid(n, 1); }

void bcat_make_progress(uint32_t status, uint32_t result, uint8_t out[BCAT_PROGRESS_SIZE]) {
    memset(out, 0, BCAT_PROGRESS_SIZE);
    memcpy(out, &status, 4);
    memcpy(out + 4, &result, 4);
}
