/* mtl_display.h - Metal side of the display pipeline (mtl_present.m, mtl_scale.m, mtl_interp.m): entry points used by the rest of the Metal layer. */
#ifndef NAPPLE_MTL_DISPLAY_H
#define NAPPLE_MTL_DISPLAY_H
#import "mtl_priv.h"
#include "display_priv.h"

/* mtl_present.m */
int mtl_present_init(void);
void mtl_stats_gpu_add(double ms);                  /* GPU time of a game submission (from its completion handler) */
/* Composes `src` (cropped by `crop`, normalised in guest units, negative height = flipped) into `target`: aspect fit, filter, upscaler. */
void mtl_present_compose(id<MTLCommandBuffer> cb, id<MTLTexture> src, const float crop[4], float guest_w, float guest_h, id<MTLTexture> target,
                         const napple_display_settings *ds, int allow_upscaler);
void mtl_present_screenshot_flush(void);
int mtl_present_metalfx_available(void);            /* 1 when the OS offers the MetalFX spatial scaler on this device */
/* mtl_upscale.m: the built-in spatial upscaler. rect = source rectangle in source pixels (x, y, w, h), dst = destination rectangle in `target` (x, y, w, h).
 * Returns 1 when encoded (0: unavailable, caller falls back to the resamplers). */
int mtl_upscale_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, const float rect[4], id<MTLTexture> target, const uint32_t dst[4], int vflip, float sharpness);

/* mtl_scale.m: render target scaling */
typedef struct mtl_scale_pick { uint32_t w, h; float rx, ry; } mtl_scale_pick;
/* Size a texture is really allocated at (== its guest size when it is not a screen-sized render target). */
void mtl_scale_pick_size(const texsnap *s, int is_depth, int for_gpu_write, mtl_scale_pick *out);
uint32_t mtl_scale_generation(void);                /* changes when the render target scale changes (existing scaled textures are rebuilt) */
void mtl_scale_tick(void);                          /* per present: re-evaluates the factors (debounced) */
void mtl_scale_note_window(uint32_t guest_w, uint32_t guest_h);   /* size of the game's swapchain textures */
void mtl_scale_count(int scaled, uint64_t bytes);
/* mtl_scale_table.m: per title rules (JSON, see the file header) */
#define MTL_SCALE_RULE_SIZES 16
typedef struct mtl_scale_size { uint32_t w, h; } mtl_scale_size;
typedef struct mtl_scale_rule {
    char title_id[20];
    int disable;
    int no_aspect_report;  /* "report_aspect": false: never tell this title the window's aspect */
    float max_scale;       /* 0 = no cap */
    uint32_t min_div;      /* 0 = default */
    mtl_scale_size skip[MTL_SCALE_RULE_SIZES], force[MTL_SCALE_RULE_SIZES];
    int nskip, nforce;
} mtl_scale_rule;
int mtl_scale_table_merge(const char *json, size_t len);
void mtl_scale_table_reset(void);
const mtl_scale_rule *mtl_scale_table_find(const char *title_id);
const mtl_scale_rule *mtl_scale_table_active(void);
int mtl_scale_rule_has(const mtl_scale_size *list, int n, uint32_t w, uint32_t h);
#endif
