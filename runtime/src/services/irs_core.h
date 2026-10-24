/* irs core: IR camera handles (portable). No Joy-Con IR camera exists on the host: handles are valid, processors cannot run. */
#pragma once
#include <stdint.h>

/* NpadIdType (0..7 = players 1..8, 0x10 other, 0x20 handheld) -> IrCameraHandle (u8 controller id, u8 npad type, 2 bytes padding);
 * returns 0 and fills *out, or -1 for an id that is not an npad. Controller ids follow the public hid numbering (handheld 8, other 9). */
int irs_camera_handle(uint32_t npad_id, uint32_t *out);
