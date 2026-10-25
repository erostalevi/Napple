/* maxwell.h - Maxwell (GM20B, SM 5.x) shader binary -> Metal Shading Language translator.
 *
 * Binding contract of the generated MSL (what the replay layer binds):
 *  - entry point `main0`. Hardware constant buffer k -> `constant uint4 *cb<k> [[buffer(k)]]` (k < 18). c0 = driver constants,
 *    c1 = compiler constants, c3+ = user uniform buffers (NVN binding i is c(i+3)).
 *  - texture slot i (ShaderInfo.tex[i], in order of first use) -> [[texture(i)]] / [[sampler(i)]]; the NVN texture handle for it
 *    is read from c2 at byte offset ShaderTexture.handle_offset (combined handles: 0x20 + 8 * binding slot).
 *  - vertex inputs `[[attribute(i)]] float4|int4|uint4 a<i>`; varyings `[[user(locn<i>)]] float4 v<i>`.
 *    Fragment outputs `[[color(rt)]]`, optional `[[depth(any)]]`.
 *  - global memory (LDG/STG) -> `device uchar *gmem [[buffer(GMEM_BUFFER_INDEX)]]`, addressed relative to maxwell_gmem_base.
 *  - shared memory (LDS/STS/ATOMS, compute only) -> a `threadgroup uint smem_w[]` declared inside the kernel (ShaderInfo.shared_mem bytes, no argument to bind);
 *    BAR is threadgroup_barrier(mem_threadgroup).
 *  - rscale table (resolution scale) -> `constant float4 *rscale [[buffer(18)]]` when ShaderInfo.uses_rscale.
 *  - clip space: ShaderKey.flip_y / depth_minus_one epilogues convert NVN clip space to Metal's.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4, STAGE_COMPUTE = 5 };
enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY, TEXTYPE_BUFFER, TEXTYPE_2D_MS };

typedef struct {
    uint8_t attr_kind[32];  /* vertex inputs: 0 float, 1 signed int, 2 unsigned int */
    uint8_t rt_kind[8];     /* fragment outputs: 0 float, 1 signed int, 2 unsigned int */
    bool depth_minus_one;   /* NVN depth mode near = -w: remap clip z to [0, w] */
    bool flip_y;            /* lower-left window origin: flip clip y */
    uint32_t depth_slots;   /* NVN combined-texture binding slots (c2 + 0x20 + 8 * slot) that hold depth textures: sampled as depth2d, result (d, 0, 0, 1) */
    bool point_prim;        /* vertex: the pipeline draws points (the point size output exists only then) */
    uint32_t vs_out_mask;   /* fragment: varyings the vertex shader provides (0 = unknown, all inputs are read) */
    uint32_t probe_pc;      /* diagnostics (NAPPLE_SHADER_PROBE): fragment shader writes registers probe_reg[] (x probe_scale) as its colour after the instruction at this pc, then exits; 0 = off */
    uint8_t probe_reg[3];
    float probe_scale;
    uint32_t shared_size;   /* compute: shared (threadgroup) memory bytes the program declares; 0 = infer from static LDS/STS offsets, 32 KB if addressed dynamically. Above 32 KB the shader is refused */
} ShaderKey;

typedef struct {
    uint32_t handle_offset; /* byte offset of the texture handle in the texture constant buffer (c2) */
    uint8_t type;           /* TEXTYPE_* */
    bool shadow, is_array, is_image, bindless, depth;
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;

typedef struct {
    int stage;
    char *msl;              /* complete MSL source (entry "main0"); NULL on failure */
    uint32_t cbuf_mask;     /* hardware constant buffers read */
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic;  /* cbufs indexed with a register: bind the whole buffer */
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;     /* vertex: input attributes; fragment: input varyings */
    uint32_t out_mask;      /* vertex: output varyings; fragment: render targets written (4 bits each) */
    bool writes_depth, uses_discard, writes_point_size;
    bool uses_gmem, uses_gmem_store;
    bool bindless_approx;       // a bindless handle indexed at run time was resolved to the first entry of its table (approximation)
    bool uses_rscale;
    uint32_t local_mem;
    uint32_t shared_mem;    /* compute: bytes of `threadgroup` storage the generated kernel declares (LDS/STS/ATOMS); 0 = none */
    uint32_t num_insts;
    char error[256];
} ShaderInfo;

#define GMEM_BUFFER_INDEX 17
#define CBSZ_BUFFER_INDEX 30     /* uint[18]: bound size in bytes of each constant buffer (register-indexed reads past it return 0) */
extern uint64_t maxwell_gmem_base, maxwell_gmem_size;   /* guest heap window addressed by global memory instructions */

/* blob: shader binary (graphics stages: 0x30-byte NVN header + 0x50-byte SPH + code; compute: code only). */
ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);
