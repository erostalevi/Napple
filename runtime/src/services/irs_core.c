#include "irs_core.h"

int irs_camera_handle(uint32_t id, uint32_t *out) {
    uint32_t controller;
    if (id <= 7) controller = id; else if (id == 0x20) controller = 8; else if (id == 0x10) controller = 9; else return -1;
    *out = controller;                                   /* npad type byte 0 */
    return 0;
}
