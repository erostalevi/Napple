/* display_priv.h - private declarations shared by display.c, mtl_present.m, mtl_scale.m and mtl_interp.m. */
#ifndef NAPPLE_DISPLAY_PRIV_H
#define NAPPLE_DISPLAY_PRIV_H
#include "../include/napple_display.h"
#ifdef __cplusplus
extern "C" {
#endif
extern napple_display_stats g_display_stats;
void gfx_scale_table_init(const char *title_id);   /* mtl_scale_table.m: per title rules (also sets the aspect report policy) */
void display_window_get(uint32_t *w, uint32_t *h, double *hz);
#ifdef __cplusplus
}
#endif
#endif
