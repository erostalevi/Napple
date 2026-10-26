#include "hidbus_core.h"
#include <string.h>

int hidbus_bus_type_valid(uint32_t t) { return t <= HIDBUS_INTERNAL_BUS; }
int hidbus_find_handle(uint32_t npad_id, uint32_t bus_type, HidbusHandle *h) {
    (void)npad_id;
    (void)bus_type;
    memset(h, 0, sizeof *h);
    return 0;
}
