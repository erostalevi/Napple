/* apm core (portable): performance configuration ids and the clocks they stand for. Handheld and docked each have their own selected
 * configuration. The table is from the author's memory of the public documentation: [unverified]; the host does not change its own speed
 * from it, so only logs and tests read the values. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t id;
    uint32_t cpu_khz, gpu_khz, emc_khz;
} ApmClocks;

/* Clocks of a known configuration id; returns 0 (and zeroes *out) for an id the table does not list. */
int apm_clocks_for(uint32_t config_id, ApmClocks *out);
/* Whether the [unverified] table marks `config_id` as a docked configuration. Documentation and logging only: the apm session accepts
 * every id in both modes (Link's Awakening sets a "docked" id for handheld mode and treats a refusal as fatal), so nothing refuses on it. */
int apm_config_docked_only(uint32_t config_id);
