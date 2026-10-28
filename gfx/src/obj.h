/* obj.h - guest-visible NVN object layouts and host-side structures shared by the API layer (C) and the Metal layer (ObjC).
 *
 * NVN objects live in memory owned by the guest and have fixed reserved sizes (nvnapi/nvn.h). Small objects are kept in place; objects the
 * Metal layer needs to find from recordings (textures, programs, descriptor pools) are referenced through (id, generation) handles so a
 * stale reference in a command list is detected instead of dereferenced. Everything here is plain C.
 */
#ifndef NAPPLE_GFX_OBJ_H
#define NAPPLE_GFX_OBJ_H

#include "internal.h"
#include "tex/napple_fmt.h"
#include "tex/napple_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " exceeds the reserved size of " #H)

/* ---------------------------------------------------------------------------------------------------------------
 * Handle table: (generation << 32 | id) -> host pointer. Lock-free reads. id 0 is never used, so a handle is never 0.
 * --------------------------------------------------------------------------------------------------------------- */
uint64_t htab_alloc(void *p);
void    *htab_get(uint64_t h);          /* NULL when the handle is stale or invalid */
void     htab_free(uint64_t h);

/* ---------------------------------------------------------------------------------------------------------------
 * Device / queue / sync / event
 * --------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t flags; } dev_builder_t;                                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags; uint32_t origin, depth_mode; void *dbg_cb, *dbg_data; } dev_t_;   FITS(dev_t_, NVNdevice);

typedef struct { NVNdevice *dev; uint32_t flags; uint32_t flush; uint32_t priority; uint64_t cmd_size, comp_size, ctl_size, mem_size; void *mem; } queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits; _Atomic uint64_t last_seq; } queue_t;    FITS(queue_t, NVNqueue);

/* Sync. kind 0: signaled when the submission timeline reaches seq. kind 1: signaled when *cell >= seq (window image released). */
typedef struct { uint32_t magic, kind; uint64_t seq; _Atomic uint64_t *cell; } sync_t;       FITS(sync_t, NVNsync);
typedef struct { const void *pool; int64_t off; } ev_builder_t;                          FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; const void *pool; int64_t off; } ev_t;   FITS(ev_t, NVNevent);

/* ---------------------------------------------------------------------------------------------------------------
 * Memory pools and buffers
 * --------------------------------------------------------------------------------------------------------------- */
typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;   FITS(pool_builder_t, NVNmemoryPoolBuilder);
typedef struct pool {
    uint32_t magic, flags;
    uint8_t *mem;                         /* CPU address == GPU address of the pool start */
    uint64_t size;
    void *mtl;                            /* gfx_mtl pool record (zero-copy MTLBuffer), or NULL */
    struct vmap *maps;                    /* virtual pools: the currently mapped ranges (see nvnMemoryPoolMapVirtual) */
    uint32_t nmaps, capmaps;
    uint8_t *vraw; uint64_t vraw_size;    /* virtual pools: the host reservation (the pool starts inside it) */
} pool_t;                                                                                   FITS(pool_t, NVNmemoryPool);

typedef struct { NVNdevice *dev; pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;     FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; pool_t *pool; int64_t off; uint64_t size; } buf_t;            FITS(buf_t, NVNbuffer);

/* ---------------------------------------------------------------------------------------------------------------
 * Textures, samplers, descriptor pools
 * --------------------------------------------------------------------------------------------------------------- */
typedef struct {
    NVNdevice *dev; napple_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
    const void *pkg_data;                 /* SetPackagedTextureData: copied into the storage at Initialize */
    uint32_t gl_name;
    uint8_t ds_mode;
} tex_builder_t;                                                                            FITS(tex_builder_t, NVNtextureBuilder);

/* A texture is its hardware-visible description: where the storage is and how to read it (what a texture descriptor holds on the
 * console). NVNtexture objects, texture-pool entries and recorded render targets all carry this snapshot; the Metal layer maps equal
 * snapshots to one Metal texture. That mirrors the hardware: a game may re-initialize a texture object over the same memory without
 * re-registering its descriptor, and the descriptor stays valid. */
typedef struct texsnap {
    napple_texdesc d;
    uint8_t *mem;                         /* storage address (GPU address == CPU address) */
    uint64_t size;                        /* storage bytes */
    uint8_t swz[4];                       /* texture-level NVNtextureSwizzle selectors */
    uint8_t ds_mode;                      /* NVNtextureDepthStencilMode */
    uint8_t valid;
    uint8_t pad[2];
} texsnap;
typedef struct { uint32_t magic, pad; texsnap s; const pool_t *pool; int64_t off; } tex_t;     FITS(tex_t, NVNtexture);   /* pool, off: as given to Initialize (GetMemoryPool / Offset) */

/* NVNtextureView is 16 bytes. */
typedef struct {
    uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, set_ds : 1, pad0 : 2;
    uint8_t base_level, nlevels, target;
    uint16_t min_layer, nlayers;
    uint16_t format;
    uint8_t swz[4];
    uint8_t ds_mode, pad1;
} view_t;
_Static_assert(sizeof(view_t) == 16, "NVNtextureView is 16 bytes");
FITS(view_t, NVNtextureView);

typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction; float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap; } samp_par;
_Static_assert(sizeof(samp_par) == 44, "sampler parameters");
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                              FITS(samp_builder_t, NVNsamplerBuilder);
typedef struct { uint32_t magic; samp_par p; } samp_t;                                       FITS(samp_t, NVNsampler);

/* Descriptor pools (NVNtexturePool / NVNsamplerPool): the guest object holds a handle to the host table of descriptors. */
typedef struct { uint32_t magic; int32_t n; const pool_t *pool; int64_t off; uint64_t handle; } dpool_t;   FITS(dpool_t, NVNtexturePool);
FITS(dpool_t, NVNsamplerPool);

/* ---------------------------------------------------------------------------------------------------------------
 * Programs
 * --------------------------------------------------------------------------------------------------------------- */
#define BARRIER_RESOLVE_DEPTH 0x80000000u   /* OP_BARRIER payload bit: nvnCommandBufferResolveDepthBuffer */
typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t hash[6]; } prog_t;      FITS(prog_t, NVNprogram);   /* hash[stage]: shader blob identity, 0 = none; a value with the high word 0x504c4348 names a stage whose code is a bare pool offset (see gfx_prog_resolve_hash) */

/* ---------------------------------------------------------------------------------------------------------------
 * Windows
 * --------------------------------------------------------------------------------------------------------------- */
typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval, active; NVNtexture *tex[4]; } win_builder_t;   FITS(win_builder_t, NVNwindowBuilder);
typedef struct {
    uint32_t magic; void *native; int32_t ntex, interval, next, active, crop[4];
    NVNtexture *tex[8];
    uint64_t presents;
    uint64_t ticket[8];                   /* presents queued per image */
    _Atomic uint64_t displayed[8];        /* presents completed (shown) per image; an image can be acquired again when displayed == ticket */
} win_t;                                                                                    FITS(win_t, NVNwindow);

/* ---------------------------------------------------------------------------------------------------------------
 * State objects: packed in place (games copy them by value)
 * --------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;               FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;                                                     FITS(chmask_st, NVNchannelMaskState);
/* adv_*: NV_blend_equation_advanced state (mode 0 = off); premultiplied source and normalized destination are the defaults, stored inverted so zero is the default */
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3, adv_mode : 6, adv_overlap : 2, adv_nonpremul : 1, adv_nonnorm : 1; } blend_st;
FITS(blend_st, NVNblendState);
/* sl_pos: programmable sample locations 0..7 (x in the low nibble, y in the high one, 1/16 pixel units, as the hardware and Metal store them); sl_n: how many were set */
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid, sl_n, sl_pos[8]; } ms_st;   FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;               FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4, fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                          FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                              FITS(va_st, NVNvertexAttribState);
typedef struct { uint32_t stride, divisor; } vs_st;                                               FITS(vs_st, NVNvertexStreamState);

/* ---------------------------------------------------------------------------------------------------------------
 * Command buffers and recordings
 * --------------------------------------------------------------------------------------------------------------- */
struct cmdrec;
/* Guest object (0xA0 bytes). The inline fast-path fields of NVN 55 headers sit at fixed offsets: the header inline functions check
 * (end - cur) before writing raw GPU methods themselves; ~0 / 0 never fits, so every call takes the API path. */
typedef struct cmdbuf {
    uint32_t magic, recording;
    struct cmdbuf_host *h;
    uint64_t inline_flags;                /* +0x10 */
    uint64_t pad18;
    uint64_t inline_cur, inline_end;      /* +0x20, +0x28 */
} cmd_t;
FITS(cmd_t, NVNcommandBuffer);
_Static_assert(__builtin_offsetof(cmd_t, inline_flags) == 0x10 && __builtin_offsetof(cmd_t, inline_cur) == 0x20, "NVN inline fast-path layout");

/* Opcodes of recorded commands. A record is { u16 op; u16 pad; u32 size } followed by the payload; size covers the header, 8-aligned. */
typedef struct { uint16_t op; uint16_t flags; uint32_t size; } op_hdr;
enum {
    OP_END = 0, OP_CALL,
    OP_BIND_PROGRAM, OP_BIND_BLEND, OP_BIND_COLOR, OP_BIND_CHMASK, OP_BIND_MS, OP_BIND_POLY, OP_BIND_DS,
    OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF, OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_IMG, OP_BIND_SEPTEX, OP_BIND_SEPSAMP,
    OP_SET_TEXPOOL, OP_SET_SAMPPOOL, OP_SET_RT,
    OP_VIEWPORTS, OP_SCISSORS, OP_DEPTH_RANGES, OP_BLEND_COLOR, OP_STENCIL, OP_POLY_OFFSET, OP_LINE_WIDTH, OP_POINT_SIZE,
    OP_PRIM_RESTART, OP_ALPHA_REF, OP_SAMPLE_MASK, OP_DEPTH_CLAMP, OP_RASTER_DISCARD, OP_DEPTH_BOUNDS, OP_RENDER_ENABLE,
    OP_CLEAR_COLOR, OP_CLEAR_DS, OP_CLEAR_TEX, OP_CLEAR_BUF,
    OP_DRAW_ARRAYS, OP_DRAW_ELEMENTS, OP_DRAW_ARRAYS_IND, OP_DRAW_ELEMENTS_IND, OP_DISPATCH, OP_DISPATCH_IND,
    OP_COPY_B2B, OP_COPY_B2T, OP_COPY_T2B, OP_COPY_T2T, OP_DOWNSAMPLE, OP_UPDATE_UBO, OP_COPY_STRIDES,
    OP_FENCE_SYNC, OP_WAIT_SYNC, OP_SIGNAL_EVENT, OP_WAIT_EVENT, OP_REPORT_COUNTER, OP_RESET_COUNTER, OP_BARRIER,
    OP_DISCARD_COLOR, OP_DISCARD_DS, OP_DRAW_TEXTURE, OP_SCREEN_MARK, OP_DRAW_IND_COUNT,
    OP_PATCH_SIZE, OP_TESS_LEVELS, OP_BIND_XFB, OP_XFB, OP_DRAW_XFB, OP_COUNT
};

/* Payloads (follow op_hdr). All pointer-sized fields are raw guest/GPU addresses or handles; nothing points into guest-owned objects. */
typedef struct { uint64_t hash[6]; uint32_t stage_mask, pad; } p_bind_program;                                /* shader blob hash per NVN stage */
typedef struct { uint32_t first, n; } p_range;                                                   /* followed by n items */
typedef struct { uint32_t stage, first, n, pad; } p_stage_range;                                 /* followed by n items */
typedef struct { uint64_t addr, size; } bufrange;
typedef struct { texsnap s; view_t view; uint8_t has_view; uint8_t pad[7]; } texref;
typedef struct { uint32_t n; uint32_t pad; texref color[8]; texref depth; } p_set_rt;
typedef struct { uint32_t index; uint32_t kind; uint32_t mask; uint32_t pad; uint32_t v[4]; } p_clear_color;   /* kind 0 f, 1 i, 2 ui */
typedef struct { float depth; uint32_t depth_mask; uint32_t stencil; uint32_t stencil_mask; } p_clear_ds;
typedef struct { uint32_t mode, first, count, base_instance, instances, pad; } p_draw_arrays;
typedef struct { uint32_t mode, idx_type, count, base_vertex, base_instance, instances; uint64_t idx_addr; } p_draw_elems;
typedef struct { uint32_t mode, idx_type; uint64_t indirect, idx_addr; } p_draw_ind;
typedef struct { uint32_t mode, idx_type, indexed, max_draws; uint64_t indirect, idx_addr, count_addr, stride; } p_draw_ind_count;   /* MultiDraw*IndirectCount */
typedef struct { uint32_t x, y, z, pad; } p_dispatch;
typedef struct { uint64_t src, dst, size; } p_copy_b2b;
typedef struct { uint32_t flags, pad; int32_t region[6]; uint64_t buf; texref tex; uint64_t row_stride, img_stride; } p_copy_xfer;   /* B2T / T2B */
typedef struct { uint32_t flags, pad; texref src, dst; int32_t sregion[6], dregion[6]; } p_copy_t2t;
typedef struct { uint64_t addr; uint32_t offset, size; } p_update_ubo;                           /* followed by size bytes */
typedef struct { uint64_t sync; uint32_t condition, flags; } p_sync;
typedef struct { uint64_t addr; uint32_t mode, value, pad; } p_event;                            /* mode 0 write, 1 add; wait: mode = NVNeventWaitMode */
typedef struct { uint32_t type, pad; uint64_t addr; } p_counter;
typedef struct { uint32_t count, pad; } p_call;
typedef struct { uint64_t handle; float dst[4], src[4]; } p_draw_tex;                            /* NVNtextureHandle, NVNdrawTextureRegion x2 */                                                  /* followed by count recording pointers */
/* tessellation and transform feedback (gfx.md section 13) */
typedef struct { uint32_t outer, pad; float v[4]; } p_tess_levels;                                /* outer = 1: 4 outer levels, 0: 2 inner levels */
enum { XFB_BEGIN = 0, XFB_END, XFB_PAUSE, XFB_RESUME };
typedef struct { uint32_t op, pad; uint64_t control; } p_xfb;                                     /* control: NVN transform feedback control memory (see mtl_stages.inc) */
typedef struct { uint32_t mode, pad; uint64_t control; } p_draw_xfb;                              /* DrawTransformFeedback: vertex count from the control memory */

/* A finished command list. Immutable once published; reference counted (owner command buffer, submissions, callers). */
typedef struct cmdrec {
    _Atomic int refs;
    uint32_t magic, len, nchildren;
    uint64_t handle;                      /* NVNcommandHandle given to the guest */
    struct cmdrec **children;             /* recordings referenced through OP_CALL (own a reference each) */
    _Atomic uint8_t dead;                 /* unused (kept for layout compatibility of diagnostics) */
    _Atomic uint32_t last_use;            /* cmdrec_now() of the last record / call / submit / read (idle recordings are held packed) */
    uintptr_t lo, hi;                     /* command memory [lo, hi) the list was recorded into (its handle's address range) */
    uint32_t reg_idx, zlen;               /* slot in the lifetime registry; packed size */
    struct cmdrec *hnext;                 /* registry hash chain (by lo) */
    _Atomic uint8_t lk;                   /* guards pack / unpack */
    uint8_t *data;                        /* raw command records, NULL while packed: read through cmdrec_data() */
    uint8_t *z;                           /* LZ4 packed copy while packed */
} cmdrec;
#define CMDREC_MAGIC MAGIC('R', 'E', 'C', '1')
void cmdrec_retain(cmdrec *r);
void cmdrec_release(cmdrec *r);
cmdrec *cmdrec_from_handle(uint64_t h);   /* NULL if stale */
const uint8_t *cmdrec_data(const cmdrec *r);   /* command records, unpacking if needed */
void gfx_cmdrec_pool_freed(const void *mem, size_t size);     /* a memory pool was finalized: lists recorded into it are gone */
uint64_t gfx_cmdrec_bytes(void);
uint64_t gfx_cmdrec_packed(void);
uint32_t cmdrec_now(void);                 /* coarse clock (seconds) for last_use */
uint64_t gfx_cmdrec_live(void);                     /* recordings alive (diagnostics, NAPPLE_PERF_LOG) */

#ifdef __cplusplus
}
#endif
#endif
