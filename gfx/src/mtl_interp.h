/* mtl_interp.h - frame generation (mtl_interp.m). */
#ifndef NAPPLE_MTL_INTERP_H
#define NAPPLE_MTL_INTERP_H
#import "mtl_display.h"

/* 1 when frame generation is enabled in the settings and this build / machine can run it. */
int mtl_interp_active(const napple_display_settings *ds);
/* Takes over the presentation of one game frame: composes it, queues it with the generated frames in between, signals the guest's `displayed` cell and the
 * timeline like the normal path. Returns 1 when it took the frame, 0 when the caller must present it normally. */
int mtl_interp_submit(const napple_display_settings *ds, id<MTLTexture> src, const float crop[4], float guest_w, float guest_h, uint32_t win_w, uint32_t win_h, double refresh_hz,
                      uint32_t interval, double speed, win_t *w, int idx, uint64_t seq, uint64_t arrival_ns);
void mtl_interp_shutdown(void);
#endif
