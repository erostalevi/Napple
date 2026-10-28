/* Connects the portable media engines (nvdrv_media_core.c) to the gfx dirty tracking: decoded frames and VIC output land in guest memory that may back a
 * texture, so gfx must be told before the host writes it (docs/architecture/gfx.md, section 3). Host (Linux) test builds do not compile this file, so
 * the hook stays NULL there. */
#include "gfx.h"
#include "nvdrv_media_core.h"

__attribute__((constructor)) static void media_gfx_glue_install(void) { media_write_prepare = gfx_guest_write_prepare; }
