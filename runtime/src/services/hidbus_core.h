/* hidbus core: bus handles (portable). No Joy-Con rail or internal-bus device exists on the host: no handle is ever "found". */
#pragma once
#include <stdint.h>

enum { HIDBUS_LEFT_JOY_RAIL = 0, HIDBUS_RIGHT_JOY_RAIL = 1, HIDBUS_INTERNAL_BUS = 2 };
/* HidbusBusHandle: u32 abstracted pad id, u8 player number, u8 bus type, u8 is_valid, u8 padding (8 bytes). */
typedef struct {
    uint32_t abstracted_pad_id;
    uint8_t player_number, bus_type, is_valid, pad;
} HidbusHandle;
int hidbus_bus_type_valid(uint32_t bus_type);
/* Looks the handle up for (npad id, bus type). Always reports "no device": returns 0 and leaves *h zeroed. */
int hidbus_find_handle(uint32_t npad_id, uint32_t bus_type, HidbusHandle *h);
