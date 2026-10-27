#include "hidbus_core.h"
#include <string.h>

int hidbus_bus_type_valid(uint32_t t) { return t <= HIDBUS_INTERNAL_BUS; }
int hidbus_find_handle(uint32_t npad_id, uint32_t bus_type, HidbusHandle *h) {
    (void)npad_id;
    (void)bus_type;
    memset(h, 0, sizeof *h);
    return 0;
}
int hidbus_handle_decode(const uint8_t *in, uint32_t n, uint32_t off, HidbusHandle *h) {
    if (!in || n < off + 8) return -1;
    memcpy(&h->abstracted_pad_id, in + off, 4);
    h->player_number = in[off + 4];
    h->bus_type = in[off + 5];
    h->is_valid = in[off + 6];
    h->pad = 0;
    return 0;
}
int hidbus_handle_attached(const HidbusHandle *h) {
    (void)h;
    return 0;
}
