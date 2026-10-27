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
 *  - surface images (SULD/SUST/SUATOM/SURED): image slot i (ShaderInfo.img[i], in order of first use) -> `texture<dim><T, access::A> im<i> [[texture(IMAGE_TEXTURE_BASE + i)]]`;
 *    the NVN image handle is read from c2 at ShaderImage.handle_offset (or from the user uniform buffer for bindless handles, as for textures). T is float/int/uint
 *    (ShaderImage.kind), A is read / write / read_write from the instructions that use the image. Atomics need MSL 3.1 (ShaderInfo.needs_msl31).
 *    Full contract: docs/architecture/gfx.md, section "Surface images".
 *  - rscale table (resolution scale) -> `constant float4 *rscale [[buffer(18)]]` when ShaderInfo.uses_rscale.
 *  - clip space: ShaderKey.flip_y / depth_minus_one epilogues convert NVN clip space to Metal's.
 *  - transform feedback (vertex stage; ShaderKey.xfb*): `device uint *xfb<k> [[buffer(XFB_BUFFER_BASE + k)]]` per captured buffer and
 *    `constant uint *xfb_ctl [[buffer(XFB_CTL_BUFFER_INDEX)]]`. ShaderKey.stage_out writes every output to `device float4 *stage_out [[buffer(STAGE_OUT_BUFFER_INDEX)]]`.
 *  - geometry (STAGE_GEOMETRY): a compute kernel, one thread per input primitive. tessellation: STAGE_TESS_CTRL is a compute kernel (one threadgroup per patch,
 *    one thread per output control point) and STAGE_TESS_EVAL a post-tessellation vertex function.
 *    Full contract (buffer indices, record layouts, dispatch shapes, what the replay side must do): docs/architecture/gfx.md, section "Geometry, tessellation and
 *    transform feedback".
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4, STAGE_COMPUTE = 5 };
enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY, TEXTYPE_BUFFER, TEXTYPE_2D_MS };

/* Stage-to-stage records: one record = STAGE_SLOTS float4. Slot 0 = system values (attribute 0x60..0x6F: x primitive id, y layer, z viewport index, w point size),
 * slot 1 = position (0x70), slot 2 + n = generic attribute n (0x80 + 16 * n). Patch records (tessellation): slot 0 = outer levels (0x00), slot 1 = inner levels
 * (0x10), slot 2 + n = generic patch attribute n (0x20 + 16 * n). */
#define STAGE_SLOTS 34
#define STAGE_SLOT_SYS 0
#define STAGE_SLOT_POS 1
#define STAGE_SLOT_GENERIC 2
/* Metal buffer indices of the stage plumbing (constant buffers use 0..17, rscale 18, CBSZ_BUFFER_INDEX 30) */
#define STAGE_IN_BUFFER_INDEX 19    /* geometry / tess control: `device const float4 *stage_in`, the assembled vertex-stage outputs (one STAGE_SLOTS record per vertex) */
#define STAGE_PARAM_BUFFER_INDEX 20 /* geometry / tess control / tess eval: `constant uint *sp`: [0] primitives (patches), [1] first input vertex record, [2] first primitive id */
#define GS_VERTEX_BUFFER_INDEX 21   /* geometry: `device float4 *gs_vtx`, gs_max_vertices records per input primitive */
#define GS_INDEX_BUFFER_INDEX 22    /* geometry: `device uint *gs_idx`, 2 * gs_max_vertices indices per input primitive, 0xFFFFFFFF = primitive restart */
#define TESS_VERTEX_BUFFER_INDEX 21 /* tess control writes / tess eval reads: `float4`, tess_out_vertices records per patch */
#define TESS_PATCH_BUFFER_INDEX 22  /* tess control writes / tess eval reads: `float4`, one patch record (STAGE_SLOTS) per patch */
#define TESS_FACTOR_BUFFER_INDEX 23 /* tess control: `device half *`, the Metal MTLQuad/TriangleTessellationFactorsHalf of each patch */
#define XFB_BUFFER_BASE 24          /* transform feedback buffer k is Metal buffer 24 + k (k < MAX_XFB_BUFFERS) */
#define XFB_CTL_BUFFER_INDEX 28     /* `constant uint *xfb_ctl`: [0] vertices per instance, [1] first vertex, [2 + k] start of xfb buffer k in dwords */
#define STAGE_OUT_BUFFER_INDEX 29   /* vertex stage with ShaderKey.stage_out: `device float4 *stage_out`, the record of vertex ordinal xo is stage_out[xo * STAGE_SLOTS ...] */
#define MAX_XFB_BUFFERS 4
#define MAX_XFB_ENTRIES 64
enum { GS_OUT_TRIANGLE_STRIP = 0, GS_OUT_POINTS = 1, GS_OUT_LINE_STRIP = 2 };
enum { TESS_DOMAIN_QUAD = 0, TESS_DOMAIN_TRIANGLE = 1, TESS_DOMAIN_ISOLINE = 2 };

/* One captured run of components: `comps` consecutive floats starting at attribute address `addr` (0x70 position, 0x80 + 16 n generic n, 0x6C point size),
 * stored at byte `offset` of every vertex of xfb buffer `buffer`. */
typedef struct {
    uint16_t addr;
    uint8_t buffer, comps;
    uint16_t offset;
} ShaderXfbEntry;

typedef struct {
    uint8_t attr_kind[32]; /* vertex inputs: 0 float, 1 signed int, 2 unsigned int */
    uint8_t rt_kind[8];    /* fragment outputs: 0 float, 1 signed int, 2 unsigned int */
    bool depth_minus_one;  /* NVN depth mode near = -w: remap clip z to [0, w] */
    bool flip_y;           /* lower-left window origin: flip clip y */
    bool upper_left;       /* fragment: the guest's window origin is upper-left (SR_Y_DIRECTION reads +1; lower-left, the default, reads -1) */
    uint32_t depth_slots;  /* NVN combined-texture binding slots (c2 + 0x20 + 8 * slot) that hold depth textures: sampled as depth2d, result (d, 0, 0, 1) */
    bool point_prim;       /* vertex: the pipeline draws points (the point size output exists only then) */
    uint32_t vs_out_mask;  /* fragment: varyings the vertex shader provides (0 = unknown, all inputs are read) */
    uint32_t
        probe_pc; /* diagnostics (NAPPLE_SHADER_PROBE): fragment shader writes registers probe_reg[] (x probe_scale) as its colour after the instruction at this pc, then exits; 0 = off */
    uint8_t probe_reg[3];
    float probe_scale;
    uint32_t
        image_int_mask; /* surface images whose texture format is signed integer (bit n = image n of the SULD/SUST immediate index; bit 31 = every bindless image) */
    uint32_t
        image_uint_mask; /* ... unsigned integer. Images in neither mask are float (or, for raw sub-word / atomic access, promoted to integer on first use) */
    uint32_t
        shared_size; /* compute: shared (threadgroup) memory bytes the program declares; 0 = infer from static LDS/STS offsets, 32 KB if addressed dynamically. Above 32 KB the shader is refused */
    /* geometry */
    uint8_t gs_in_vertices;   /* vertices of one input primitive: 1 points, 2 lines, 3 triangles, 4 lines with adjacency, 6 triangles with adjacency (0 = 3) */
    uint8_t gs_out_topology;  /* GS_OUT_*: what the emitted vertices form (the replay draws gs_idx with this topology) */
    uint16_t gs_max_vertices; /* declared maximum output vertices per input primitive (required for STAGE_GEOMETRY) */
    /* tessellation */
    uint8_t tess_in_vertices;  /* tess control: control points per input patch (1..32) */
    uint8_t tess_out_vertices; /* tess control: output control points per patch = invocations per patch (1..32); tess eval: the same value (its input patch size) */
    uint8_t tess_domain;       /* TESS_DOMAIN_*: tess control (selects the factor record) and tess eval (selects position_in_patch). Isolines are refused */
    /* vertex stage capture */
    bool stage_out;         /* vertex: write every output to the stage-out buffer (the input of a geometry or tess control stage) instead of leaving it to the rasterizer */
    uint8_t xfb_count;      /* vertex: number of valid xfb[] entries */
    uint16_t xfb_stride[MAX_XFB_BUFFERS]; /* bytes per captured vertex of each buffer (a multiple of 4) */
    ShaderXfbEntry xfb[MAX_XFB_ENTRIES];
} ShaderKey;

typedef struct {
    uint32_t handle_offset; /* byte offset of the texture handle in the texture constant buffer (c2) */
    uint8_t type;           /* TEXTYPE_* */
    bool shadow, is_array, is_image, bindless, depth;
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;

enum { IMGACC_READ = 1, IMGACC_WRITE = 2 };
enum { IMGKIND_FLOAT = 0, IMGKIND_INT = 1, IMGKIND_UINT = 2 };
#define IMAGE_TEXTURE_BASE 64 /* Metal texture index of image slot 0 (sampled textures use 0..62) */
#define MAX_IMAGES 8
typedef struct {
    uint32_t handle_offset; /* byte offset of the image handle in c2 (immediate index * 4) */
    uint8_t type;           /* TEXTYPE_1D, _1D_ARRAY, _2D, _2D_ARRAY, _3D or _BUFFER */
    uint8_t access;         /* IMGACC_* bits */
    uint8_t kind;           /* IMGKIND_* : component type of the Metal texture */
    bool kind_fixed;        /* kind came from ShaderKey (or an atomic forced it) */
    bool atomic;            /* used by SUATOM/SURED: the texture must be r32uint / r32sint with MTLTextureUsageShaderAtomic */
    bool bindless;
    uint32_t bindless_cbuf, bindless_offset;
} ShaderImage;

typedef struct {
    int stage;
    char *msl;          /* complete MSL source (entry "main0"); NULL on failure */
    uint32_t cbuf_mask; /* hardware constant buffers read */
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic; /* cbufs indexed with a register: bind the whole buffer */
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;      /* vertex: input attributes; fragment: input varyings */
    uint32_t out_mask;       /* vertex: output varyings; fragment: render targets written (4 bits each) */
    uint32_t attr_read_mask; /* fragment: every varying the program reads by a constant address, provided by the vertex shader or not */
    bool indexed_inputs;     /* fragment: varyings read through a register (the generated code then depends on the whole vs_out_mask) */
    bool writes_depth, uses_discard, writes_point_size;
    bool uses_gmem, uses_gmem_store;
    int nimg;
    ShaderImage img[MAX_IMAGES];
    bool needs_msl31;       /* the MSL uses texture atomics: compile with MTLLanguageVersion3_1 */
    bool bindless_approx;   // a bindless handle indexed at run time was resolved to the first entry of its table (approximation)
    bool uses_rscale;
    uint32_t local_mem;
    uint32_t shared_mem; /* compute: bytes of `threadgroup` storage the generated kernel declares (LDS/STS/ATOMS); 0 = none */
    uint32_t num_insts;
    /* stages beyond vertex / fragment / compute */
    uint64_t stage_slot_mask; /* geometry / tess: STAGE_SLOT bits the program stores (the replay copies / binds only these); tess control: patch slots too, see patch_slot_mask */
    uint64_t patch_slot_mask; /* tess control: patch record slots written */
    uint32_t xfb_buffer_mask; /* vertex: xfb buffers the program writes */
    uint16_t gs_max_vertices;
    uint8_t gs_out_topology, tess_domain, tess_out_vertices;
    bool uses_stage_out;      /* the program writes the stage-out buffer */
    bool uses_xfb;
    char error[256];
} ShaderInfo;

#define GMEM_BUFFER_INDEX 17
#define CBSZ_BUFFER_INDEX 30                          /* uint[18]: bound size in bytes of each constant buffer (register-indexed reads past it return 0) */
extern uint64_t maxwell_gmem_base, maxwell_gmem_size; /* guest heap window addressed by global memory instructions */

/* blob: shader binary (graphics stages: 0x30-byte NVN header + 0x50-byte SPH + code; compute: code only). */
ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);
