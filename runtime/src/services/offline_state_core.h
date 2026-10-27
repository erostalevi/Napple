/* offline state core: the small pieces of state the offline services keep (portable): mm:u clock requests and the prepo report store. These are the
 * parts of the "no hardware / no network" services that are not just an error answer, so they are held here and unit tested without IPC. */
#pragma once
#include <stdint.h>

/* ---- mm:u (nn::mmnv): multimedia clock requests -------------------------------------------------------------------------------- */
#define MM_MAX_REQUESTS 32
#define MM_MAX_MODULES 16
typedef struct {
    struct {
        uint8_t used;
        uint32_t module, min;
        int32_t max;
    } req[MM_MAX_REQUESTS];
    struct { /* the legacy by-module interface keeps one state per module */
        uint8_t used;
        uint32_t min;
        int32_t max;
    } mod[MM_MAX_MODULES];
} MmTable;
void mm_init(MmTable *);
uint32_t mm_request_open(MmTable *, uint32_t module);                   /* request id (>= 1), 0 when the table is full */
void mm_request_close(MmTable *, uint32_t id);                          /* unknown ids are ignored */
void mm_request_set(MmTable *, uint32_t id, uint32_t min, int32_t max); /* the clock is "applied" at once: there is no hardware to wait for */
uint32_t mm_request_get(const MmTable *, uint32_t id);                  /* the rate last set; 0 for an unknown or unset request */
void mm_module_open(MmTable *, uint32_t module);
void mm_module_close(MmTable *, uint32_t module);
void mm_module_set(MmTable *, uint32_t module, uint32_t min, int32_t max);
uint32_t mm_module_get(const MmTable *, uint32_t module);

/* ---- prepo: play report store ---------------------------------------------------------------------------------------------- */
typedef struct {
    uint64_t bytes;          /* payload bytes accepted since the last ClearStorage */
    uint32_t count;          /* reports accepted */
    uint8_t agreement_check; /* IsUserAgreementCheckEnabled; defaults to enabled, like a console */
} PrepoStore;
#define PREPO_STORAGE_LIMIT (4u << 20) /* a console keeps a few MB of unsent reports; past that reports are dropped */
void prepo_init(PrepoStore *);
void prepo_account(PrepoStore *, uint64_t n); /* a report of n payload bytes arrived (dropped past the limit) */
void prepo_clear(PrepoStore *);
