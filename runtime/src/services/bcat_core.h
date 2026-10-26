/* bcat core: delivery-cache names and progress record (portable). The host has no delivery cache: every directory is absent. */
#pragma once
#include <stdint.h>

#define BCAT_NAME_SIZE 0x20u
#define BCAT_PROGRESS_SIZE 0x200u
enum {
    BCAT_STATUS_NONE = 0,
    BCAT_STATUS_QUEUED = 1,
    BCAT_STATUS_CONNECTING = 2,
    BCAT_STATUS_PROCESSING_DATA_LIST = 3,
    BCAT_STATUS_DOWNLOADING = 4,
    BCAT_STATUS_COMMITTING = 5,
    BCAT_STATUS_DONE = 6
};

/* A directory name is 1..31 characters of [A-Za-z0-9_-], NUL padded. A file name additionally allows '.'. */
int bcat_valid_directory_name(const char name[BCAT_NAME_SIZE]);
int bcat_valid_file_name(const char name[BCAT_NAME_SIZE]);
/* DeliveryCacheProgressImpl: u32 status at +0, u32 result at +4, everything else 0 (nothing was transferred). */
void bcat_make_progress(uint32_t status, uint32_t result, uint8_t out[BCAT_PROGRESS_SIZE]);
