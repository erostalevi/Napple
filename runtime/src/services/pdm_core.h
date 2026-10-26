/* pdm core: play statistics (portable). The host only knows about the running title: its play time is the time since launch. */
#pragma once
#include <stdint.h>

#define PDM_STATS_SIZE 0x18u
/* PlayStatistics: u64 application id, u32 first entry index, u32 last entry index, u32 play time in minutes, u32 launch count. */
typedef struct {
    uint64_t application_id;
    uint32_t first_entry, last_entry, play_minutes, launch_count;
} PdmStats;
_Static_assert(sizeof(PdmStats) == PDM_STATS_SIZE, "PlayStatistics is 0x18 bytes");

/* Statistics for `app_id`: the running title (own_app_id) gets one launch and the minutes elapsed since start (guest ticks at 19.2 MHz);
 * any other title has never been played. */
void pdm_stats_for(uint64_t app_id, uint64_t own_app_id, uint64_t guest_ticks_since_start, PdmStats *out);
