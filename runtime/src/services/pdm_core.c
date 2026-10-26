#include "pdm_core.h"
#include <string.h>

void pdm_stats_for(uint64_t app_id, uint64_t own, uint64_t ticks, PdmStats *out) {
    memset(out, 0, sizeof *out);
    out->application_id = app_id;
    if (app_id != own) return;
    uint64_t minutes = ticks / 19200000ull / 60ull;
    out->play_minutes = minutes > 0xffffffffull ? 0xffffffffu : (uint32_t)minutes;
    out->launch_count = 1;
}
