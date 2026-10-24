/* gfx_mtl.h - the boundary between the NVN API layer (C) and the Metal layer (Objective-C). Plain C declarations. */
#ifndef NAPPLE_GFX_MTL_H
#define NAPPLE_GFX_MTL_H

#include "hostobj.h"

#ifdef __cplusplus
extern "C" {
#endif

int  gfx_mtl_init(void *metal_layer, uint32_t headless_w, uint32_t headless_h);
void gfx_mtl_shutdown(void);
void gfx_mtl_settings_changed(void);
void gfx_mtl_set_gmem(uint64_t base, uint64_t size);   /* guest heap window that shaders address through global memory instructions */

/* Memory pools: the pool memory is wrapped (no copy) so GPU addresses equal CPU addresses. */
void *gfx_mtl_pool_add(uint8_t *mem, uint64_t size, uint32_t flags);
void  gfx_mtl_pool_remove(void *rec);

/* Textures are created lazily by the replay thread from their snapshot; this drops Metal textures whose storage lies inside a range
 * (a pool was finalized) or marks a texture as written by the CPU through the API (WriteTexels). */
void gfx_mtl_tex_invalidate(const texsnap *t);

/* Shader blobs: the API layer hands the code of every program stage to the shader module, which translates it in the background. */
uint64_t gfx_shader_register(int stage, uint64_t code_addr, const uint8_t *control);   /* returns the blob hash (0 on failure) */

/* Submission. Each call returns the timeline value that completes when the GPU finished the work (monotonic, 1-based). */
uint64_t gfx_mtl_submit(cmdrec *const *recs, uint32_t n);
uint64_t gfx_mtl_present(win_t *w, int tex_index, const texsnap *t, uint32_t interval);
uint64_t gfx_mtl_queue_wait(uint32_t kind, uint64_t seq, _Atomic uint64_t *cell);   /* in-order wait job (QueueWaitSync) */
void     gfx_mtl_flush(void);               /* make sure everything submitted starts executing */
void     gfx_mtl_finish(void);              /* wait for everything submitted so far */

/* Window textures: the Metal texture of a window image is created eagerly so presentation never races with creation. */
void gfx_mtl_window_attach(win_t *w);

/* Dirty-page tracking of texture storage (dirty.c). */
void     mtl_pipe_prewarm(uint64_t blob_hash);                        /* a program was registered: build persisted pipelines that use it */
void     dirty_add_range(const void *mem, size_t size);
void     dirty_remove_range(const void *mem, size_t size);          /* same (mem, size) as the matching add */
void     dirty_invalidate(const void *mem, size_t size);            /* the mapping under the range was replaced: drop tracking state, bump generations */
int      dirty_describe(const void *addr, char *buf, size_t n);     /* diagnostics */
uint64_t dirty_now(void);
int      dirty_since(const void *addr, size_t len, uint64_t stamp);   /* any covered page written after `stamp` (or untracked) */
void     dirty_protect(const void *addr, size_t len);                  /* write-protect covered tracked pages */
int      dirty_tracked(const void *addr, size_t len);

/* Texel transfer helpers shared by API and Metal layers (tex_xfer.c). */
int  tex_write_texels(const texsnap *t, unsigned level, unsigned layer, const int32_t region[6], const void *src, size_t row_stride, size_t img_stride);
int  tex_read_texels(const texsnap *t, unsigned level, unsigned layer, const int32_t region[6], void *dst, size_t row_stride, size_t img_stride);

#ifdef __cplusplus
}
#endif
#endif
