#include "apm_core.h"
#include <string.h>

/* [unverified] values in kHz. Handheld ids 0x0001xxxx/0x0002xxxx, docked ids 0x0002xxxx/0x9222xxxx. */
static const struct {
    ApmClocks c;
    int docked_only;
} table[] = {
    {{0x00010000, 1020000, 307200, 1331200}, 0}, {{0x00010001, 1020000, 384000, 1331200}, 0}, {{0x00010002, 1020000, 768000, 1600000}, 0},
    {{0x00020000, 1020000, 307200, 1331200}, 0}, {{0x00020001, 1020000, 384000, 1331200}, 0}, {{0x00020002, 1020000, 460800, 1600000}, 0},
    {{0x00020003, 1020000, 768000, 1600000}, 1}, {{0x00020004, 1020000, 921600, 1600000}, 1}, {{0x00020005, 1785000, 307200, 1331200}, 0},
    {{0x00020006, 1785000, 768000, 1600000}, 1}, {{0x92220007, 1020000, 307200, 1600000}, 1}, {{0x92220008, 1020000, 460800, 1600000}, 1},
    {{0x92220009, 1785000, 768000, 1600000}, 1}, {{0x9222000a, 1020000, 921600, 1600000}, 1},
};

int apm_clocks_for(uint32_t id, ApmClocks *out) {
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
        if (table[i].c.id == id) {
            *out = table[i].c;
            return 1;
        }
    memset(out, 0, sizeof *out);
    return 0;
}

int apm_config_docked_only(uint32_t id) {
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
        if (table[i].c.id == id) return table[i].docked_only;
    return 0; /* an id the table does not know: titles ship ids newer than this table */
}
