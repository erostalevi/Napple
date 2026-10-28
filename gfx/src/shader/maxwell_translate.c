// Maxwell (SM 5.x) shader binary -> Metal Shading Language.
//
// The binary is an NVN header (0x30), the shader program header (SPH, 0x50; graphics stages), then
// instructions in groups of four 64-bit words (one scheduling word + three instructions).
// Registers are emitted as `uint` variables; float ops use as_type<> conversions. Shaders with
// branches become a `switch (pc)` state machine with explicit SSY/PBK/PCNT stacks.
//
#if defined(_WIN32)
#include <malloc.h>   // alloca
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maxwell.h"
#include "soft_double_msl.h"

uint64_t maxwell_gmem_base, maxwell_gmem_size;

#define RZ 255
#define LMEM_WORDS 256   // local memory per invocation (1 KB)
#define PT 7

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

typedef struct {
    const uint8_t *code;   // first instruction group
    size_t ninsts;         // number of 8-byte words to scan
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    Str body;
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask;
    uint32_t vs_in_comp[32];
    bool uses_branches, uses_cc, uses_local, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_lane, uses_point_coord, uses_attr_arr;
    bool uses_rscale;              // resolution-scale table (fragment/compute only)
    uint32_t flat_mask;            // fragment inputs using constant (flat) interpolation
    uint32_t noperspective_mask;   // fragment inputs using pass (linear) interpolation
    uint32_t omap_target, omap_depth;
    int tex_slot[8192];   // texture index -> Metal slot + 1
    bool tex_weak[64];    // slot type only guessed (from a size query): the next sample decides
    bool fail;
    bool branchy;           // program uses the pc state machine
    int ipa_mode;           // interpolation mode of the IPA being translated (-1 for plain loads)
    int ipa_mul;            // its multiplier register (mode 1: the result is the "pass" value attr/w times this register)
    bool uses_gmem_store;   // STG/ATOM/RED present: the gmem buffer is written
    bool uses_f64;          // a double-precision op is present: the emulation library (soft_double_core.inc) is emitted
    bool uses_smem, smem_dyn, atom_dev, atom_tg,
        uses_vattr_arr;       // shared memory in use / addressed through a register / atomics / vertex inputs read through a register
    uint32_t smem_need;       // highest statically addressed shared byte + 1
    uint32_t cbuf_need[18];   // highest constant vec4 index + 1 per cbuf
    int reg_cb[256];          // register -> (cbuf << 24 | byte offset) it was last loaded from (-1 = unknown): resolves bindless (.B) texture handles
    int bl_n, bl_cb[64], bl_off[64], bl_slot[64];   // bindless handles read from user uniform buffers (index 8192 + n in tex_slot)
    uint64_t gs_slots;      // geometry / tess control: record slots stored (STAGE_SLOT bits); tess control: patch slots in patch_slots
    uint64_t patch_slots;
    bool uses_vsys;         // vertex: system value outputs (attribute 0x60..0x6F) are kept in a local for stage_out / xfb
    bool uses_gs_emit;      // geometry: OUT present
    bool uses_vabsdiff4, uses_isel, uses_idp;   // helper functions to emit
    bool uses_stencil;                          // a texture binding is a stencil view: uint texture + the StencilTex wrapper
    bool has_pexit, uses_ljs, uses_lmb;         // PEXIT in the program / PLONGJMP stack and local memory base to declare
    int ald_vertex;         // register holding the vertex index of the ALD/AST being translated (RZ = none), set by the handler
    bool ald_patch, ald_out, ald_vlatched;   // ald_vlatched: the vertex index was copied to `vx` (the destination registers may overlap it)
} Ctx;

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1)); }
static int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}

static void fail(Ctx *c, const char *fmt, ...) {
    if (c->fail) return;
    c->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->error, sizeof c->info->error, fmt, ap);
    va_end(ap);
}

// Branch targets may point at a scheduling word; execution continues at the next instruction.
static uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

// ------------------------------------------------------------------ operands
static const char *reg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    sprintf(buf, "r%d", r);
    return buf;
}

#define R(r) reg(c, (r), alloca(16))
#define F(expr) fmt_f(alloca(512), (expr))
static char *fmt_f(char *b, const char *e) {
    sprintf(b, "as_type<float>(%s)", e);
    return b;
}

static const char *pred(Ctx *c, int p, bool neg, char *buf) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    sprintf(buf, "%sp%d", neg ? "!" : "", p);
    return buf;
}
#define P(p, n) pred(c, (p), (n), alloca(16))

static const char *cbuf_ref(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    if (off + 16 > (int64_t)c->info->cbuf_size[idx]) c->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) c->info->cbuf_dynamic |= 1u << idx;
    if (!dyn && idx < 18 && off >= 0 && (uint32_t)(off >> 4) + 1 > c->cbuf_need[idx]) c->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
    if (dyn) sprintf(buf, off < 0 ? "cbr(cb%d, cbsz[%d], (%s) - %lluu)" : "cbr(cb%d, cbsz[%d], (%s) + %lluu)", idx, idx, dyn, (unsigned long long)(off < 0 ? -off : off));
    else if (off < 0) sprintf(buf, "0u /* c[%d][%lld] */", idx, (long long)off);   /* a negative offset without a register: nothing to read */
    else sprintf(buf, "cb%d[%lld][%lld]", idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// Operand B in its register / constant buffer / immediate forms, selected by the opcode class.
typedef enum { B_REG, B_CBUF, B_IMM } BForm;

static BForm b_form(uint64_t w) {
    uint64_t top = w >> 60;
    if (top == 0x5) return B_REG;
    if (top == 0x4) return B_CBUF;
    return B_IMM;
}

static const char *operand_b(Ctx *c, uint64_t w, bool float_imm, char *buf) {
    switch (b_form(w)) {
        case B_REG: return reg(c, (int)bits(w, 20, 8), buf);
        case B_CBUF: return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
        default: {
            uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
            if (float_imm) v <<= 12;
            else if (v & 0x80000u) v |= 0xFFF00000u;   // sign-extend 20 bits
            sprintf(buf, "0x%08xu", v);
            return buf;
        }
    }
}
#define B(fi) operand_b(c, w, (fi), alloca(128))

// The "C" operand of three-source ops: REG_39, or the cbuf/reg swap form (0x51/0x53/... classes).
static const char *operand_c_for_ffma(Ctx *c, uint64_t w, char *buf, const char **b_out, char *bbuf) {
    uint64_t op8 = w >> 56;
    if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {   // e.g. 0x51: B = REG_39, C = cbuf
        *b_out = reg(c, (int)bits(w, 39, 8), bbuf);
        return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    }
    *b_out = operand_b(c, w, true, bbuf);
    return reg(c, (int)bits(w, 39, 8), buf);
}

static void set_reg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = 1;
    c->reg_cb[r] = -1;
    sp(&c->body, "    r%d = %s;\n", r, expr);
}

static const char *fneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", e);
    else if (abs_) sprintf(buf, "abs(%s)", e);
    else if (neg) sprintf(buf, "(-%s)", e);
    else return e;
    return buf;
}
#define NA(e, n, a) fneg_abs((e), (n), (a), alloca(600))

static const char *fresult(const char *e, bool sat, char *buf) {
    if (sat) sprintf(buf, "as_type<uint>(saturate(%s))", e);
    else sprintf(buf, "as_type<uint>(%s)", e);
    return buf;
}
#define FRES(e, sat) fresult((e), (sat), alloca(700))

// ------------------------------------------------------------------ double precision
// Metal has no double: a binary64 is a register pair (Rn = low word, Rn+1 = high word, n even) carried as a `ulong` of raw bits, and every operation is a call
// into the library of soft_double_core.inc (bit-exact IEEE 754, all four rounding modes, emitted once per shader that needs it).
// The emulation level: NAPPLE_F64=off refuses double-precision shaders again (the pre-emulation behaviour, for A/B debugging of a suspect shader); anything else,
// and the default, is the bit-exact emulation.
static void use_f64(Ctx *c) {
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("NAPPLE_F64");
        off = e && !strcmp(e, "off");
    }
    if (off) fail(c, "double precision is disabled (NAPPLE_F64=off)");
    c->uses_f64 = true;
}

static const char *dpair(Ctx *c, int r, char *buf) {
    use_f64(c);
    if (r == RZ) return "0ul";
    char lo[16], hi[16];
    sprintf(buf, "(ulong(%s) | (ulong(%s) << 32))", reg(c, r, lo), reg(c, r + 1, hi));
    return buf;
}

static const char *dcbuf(Ctx *c, uint64_t w, char *buf) {
    char l[128], h[128];
    int idx = (int)bits(w, 34, 5);
    int64_t off = (int64_t)bits(w, 20, 14) * 4;
    sprintf(buf, "(ulong(%s) | (ulong(%s) << 32))", cbuf_ref(c, idx, off, NULL, l), cbuf_ref(c, idx, off + 4, NULL, h));
    return buf;
}

// operand B as a 64-bit value: register pair, two constant-buffer words, or the 20-bit immediate that fills the top of the high word
static const char *dop_b(Ctx *c, uint64_t w, char *buf) {
    use_f64(c);
    switch (b_form(w)) {
        case B_REG: return dpair(c, (int)bits(w, 20, 8), buf);
        case B_CBUF: return dcbuf(c, w, buf);
        default: {
            uint32_t v = ((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19)) << 12;
            sprintf(buf, "0x%08x00000000ul", v);
            return buf;
        }
    }
}
#define DB() dop_b(c, w, alloca(320))
#define DR(r) dpair(c, (r), alloca(96))

// DFMA's operands: B and C swap places between the register form (C = REG_39) and the 0x53 form (B = REG_39, C = constant buffer)
static const char *dop_c_dfma(Ctx *c, uint64_t w, char *cbuf_, const char **b_out, char *bbuf) {
    if ((w >> 56) == 0x53) {
        *b_out = dpair(c, (int)bits(w, 39, 8), bbuf);
        return dcbuf(c, w, cbuf_);
    }
    *b_out = dop_b(c, w, bbuf);
    return dpair(c, (int)bits(w, 39, 8), cbuf_);
}

static const char *dneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(sd_abs(%s) ^ 0x8000000000000000ul)", e);
    else if (abs_) sprintf(buf, "sd_abs(%s)", e);
    else if (neg) sprintf(buf, "sd_neg(%s)", e);
    else return e;
    return buf;
}
#define DNA(e, n, a) dneg_abs((e), (n), (a), alloca(700))

static void dstore(Ctx *c, int d, const char *expr) {
    if (d == RZ) return;
    sp(&c->body, "    { ulong dv = %s;\n", expr);
    set_reg(c, d, "uint(dv)");
    if (d + 1 < RZ) set_reg(c, d + 1, "uint(dv >> 32)");
    sp(&c->body, "    }\n");
}

static const char *cmp_op(int op) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "num", "nan", "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[op & 15];
}

// float comparison including unordered variants
static void fcmp_expr(char *out, int op, const char *a, const char *b) {
    switch (op) {
        case 0: strcpy(out, "false"); break;
        case 15: strcpy(out, "true"); break;
        case 7: sprintf(out, "(!isnan(%s) && !isnan(%s))", a, b); break;
        case 8: sprintf(out, "(isnan(%s) || isnan(%s))", a, b); break;
        case 5: sprintf(out, "((%s < %s) || (%s > %s))", a, b, a, b); break;   // ordered NE: false when either is NaN (MSL `!=` is true then)
        default:
            if (op < 7) sprintf(out, "(%s %s %s)", a, cmp_op(op), b);
            else sprintf(out, "((%s %s %s) || isnan(%s) || isnan(%s))", a, cmp_op(op), b, a, b);
    }
}

static const char *bool_op(int op) { return op == 0 ? "&&" : op == 1 ? "||" : "!="; }

static void icmp_expr(char *out, int op, const char *a, const char *b, bool is_signed) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) strcpy(out, "false");
    else if (op == 7) strcpy(out, "true");
    else if (is_signed) sprintf(out, "(int(%s) %s int(%s))", a, ops[op], b);
    else sprintf(out, "(%s %s %s)", a, ops[op], b);
}

// ------------------------------------------------------------------ attributes
// Maxwell attribute space: 0x70 position, 0x80 + 16*i generic attribute i, 0x2F8 instance id,
// 0x2FC vertex id, 0x3FC front facing, 0x6C point size.
/* Stage-to-stage record slot of an attribute address (-1: not stored in a record). Patch records use their own map. */
static int stage_slot(uint32_t addr) {
    if (addr >= 0x60 && addr < 0x70) return STAGE_SLOT_SYS;
    if (addr >= 0x70 && addr < 0x80) return STAGE_SLOT_POS;
    if (addr >= 0x80 && addr < 0x280) return STAGE_SLOT_GENERIC + (int)((addr - 0x80) >> 4);
    return -1;
}
static int patch_slot(uint32_t addr) {
    if (addr < 0x10) return 0;
    if (addr < 0x20) return 1;
    if (addr < 0x220) return 2 + (int)((addr - 0x20) >> 4);
    return -1;
}

static int gs_in_verts(const Ctx *c) { return c->key && c->key->gs_in_vertices ? c->key->gs_in_vertices : 3; }

/* ALD in the geometry, tess control and tess eval stages. `vtx` is the vertex index register (RZ: vertex 0), `idx` the run-time attribute address expression of
 * an indexed read (NULL for a constant address). */
static void attr_load_staged(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[400], vtx[40], sl[160], cm[160];
    const char *vexp = c->ald_vertex == RZ ? "0u" : c->ald_vlatched ? "vx" : reg(c, c->ald_vertex, vtx);
    bool patch = c->ald_patch;
    if (idx) {
        if (patch) sprintf(sl, "min(((%s) < 0x20u ? ((%s) >> 4) : 2u + (((%s) - 0x20u) >> 4)), %uu)", idx, idx, idx, STAGE_SLOTS - 1);
        else sprintf(sl, "min(((%s) < 0x70u ? 0u : ((%s) < 0x80u ? 1u : 2u + (((%s) - 0x80u) >> 4))), %uu)", idx, idx, idx, STAGE_SLOTS - 1);
        sprintf(cm, "((%s) >> 2) & 3u", idx);
    } else if (c->stage == STAGE_TESS_EVAL && !patch && (addr == 0x2F0 || addr == 0x2F4)) {
        sprintf(e, "as_type<uint>(tess_coord[%d])", (addr >> 2) & 1);
        set_reg(c, dst, e);
        return;
    } else {
        int sv = patch ? patch_slot(addr) : stage_slot(addr);
        if (sv < 0) {
            sprintf(e, "0u /* attr %#x */", addr);
            set_reg(c, dst, e);
            return;
        }
        sprintf(sl, "%uu", sv);
        sprintf(cm, "%uu", (addr >> 2) & 3);
        if (!patch && sv >= STAGE_SLOT_GENERIC) c->in_attr_mask |= 1u << (sv - STAGE_SLOT_GENERIC);
    }
    if (idx && !patch) c->in_attr_mask = 0xffffffffu;
    if (c->stage == STAGE_GEOMETRY) {
        if (!idx && addr == 0x60 && !patch) {
            sprintf(e, "(sp[2] + gs_pid)");
        } else if (patch) {
            sprintf(e, "0u /* patch attribute in a geometry shader */");
        } else {
            sprintf(e, "as_type<uint>(stage_in[(sp[1] + gs_pid * %uu + %s) * %uu + %s][%s])", gs_in_verts(c), vexp, STAGE_SLOTS, sl, cm);
        }
    } else if (c->stage == STAGE_TESS_CTRL) {
        if (!idx && addr == 0x60 && !patch) sprintf(e, "(sp[2] + pid)");
        else if (patch) sprintf(e, "as_type<uint>(tess_patch[pid * %uu + %s][%s])", STAGE_SLOTS, sl, cm);
        else if (c->ald_out) sprintf(e, "as_type<uint>(tess_vtx[(pid * %uu + %s) * %uu + %s][%s])", c->key ? c->key->tess_out_vertices : 1u, vexp, STAGE_SLOTS, sl, cm);
        else sprintf(e, "as_type<uint>(stage_in[(sp[1] + pid * %uu + %s) * %uu + %s][%s])", c->key ? c->key->tess_in_vertices : 1u, vexp, STAGE_SLOTS, sl, cm);
    } else {   // STAGE_TESS_EVAL
        if (!idx && addr == 0x60 && !patch) strcpy(e, "(sp[2] + patch_id)");
        else if (patch) sprintf(e, "as_type<uint>(tess_patch[patch_id * %uu + %s][%s])", STAGE_SLOTS, sl, cm);
        else sprintf(e, "as_type<uint>(tess_vtx[(patch_id * %uu + %s) * %uu + %s][%s])", c->key ? c->key->tess_out_vertices : 1u, vexp, STAGE_SLOTS, sl, cm);
    }
    set_reg(c, dst, e);
}

static void attr_load(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[256];
    if (c->stage == STAGE_GEOMETRY || c->stage == STAGE_TESS_CTRL || c->stage == STAGE_TESS_EVAL) {
        attr_load_staged(c, dst, addr, idx);
        return;
    }
    if (idx) {
        /* Interpolated input read through a register address (AL2P + IPA with the index flag; decal_uber): all varyings the vertex shader provides are
         * mirrored in a local array and the register (a physical attribute address, 0x80 + 16 * n + 4 * comp) selects the element. */
        if (c->stage == STAGE_VERTEX) {
            /* vertex inputs read through a register (a[Ra + imm]): the declared inputs are mirrored in a local table in the prologue. Only attributes the
             * program also names statically are declared; any other index reads 0. */
            c->uses_vattr_arr = true;
            sprintf(e, "(((%s) >= 0x80u && (%s) < 0x280u) ? vattr_arr[((%s) - 0x80u) >> 2] : 0u)", idx, idx, idx);
            set_reg(c, dst, e);
            return;
        }
        if (c->stage != STAGE_FRAGMENT) {
            fail(c, "indexed attribute load outside the vertex and fragment stages");
            return;
        }
        c->in_attr_mask |= (c->key && c->key->vs_out_mask) ? c->key->vs_out_mask : 0xffffffffu;
        c->uses_attr_arr = true; /* mask 0 = unknown: every varying */
        char el[200];
        sprintf(el, "attr_arr[min((%s - 0x80u) >> 2, 127u)]", idx);
        if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(%s * in.pos.w)", el);
        else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
            char mb[16];
            sprintf(e, "as_type<uint>(%s * in.pos.w * as_type<float>(%s))", el, reg(c, c->ipa_mul, mb));
        } else sprintf(e, "as_type<uint>(%s)", el);
        set_reg(c, dst, e);
        return;
    }
    if (c->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->in_attr_mask |= 1u << a;
            c->vs_in_comp[a] |= 1u << comp;
            int kind = c->key ? c->key->attr_kind[a] : 0;
            if (kind == 0) sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
            else sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
        } else if (addr == 0x2F8) {
            c->uses_iid = true;
            strcpy(e, "iid");
        } else if (addr == 0x2FC) {
            c->uses_vid = true;
            strcpy(e, "vid");
        } else {
            sprintf(e, "0u /* attr %#x */", addr);
        }
    } else if (c->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            c->uses_frag_coord = true;
            int comp = (addr >> 2) & 3;
            if (comp < 2) {   // guest pixel units: the render target may be allocated larger
                c->uses_rscale = true;
                sprintf(e, "as_type<uint>((in.pos[%d] - rscale[0][%d]) / rscale[0][%d])", comp, comp + 2, comp);
            } else {
                sprintf(e, "as_type<uint>(in.pos[%d])", comp);
            }
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->info->attr_read_mask |= 1u << a; /* read whether or not the vertex shader provides it (pipeline key normalisation, mtl_shader.m) */
            // (MSL: a mask of 0 means unknown. GLSL: the renderer always passes the vertex shader's outputs, and
            // Vulkan requires every fragment input to be a vertex output, so 0 means none.)
            if (c->key && c->key->vs_out_mask && !(c->key->vs_out_mask & (1u << a))) {
                strcpy(e, comp == 3 ? "0x3f800000u" : "0u");   // not provided by the vertex shader
            } else {
                c->in_attr_mask |= 1u << a;
                // IPA "pass" yields attr/w (the shader multiplies by w itself); Metal gives attr. "multiply" is the
                // pass value times a register - usually w (from the pass of position w: Metal's attr again), but a
                // shader may pass its own (the Mysterious Woods mist: 1/(v4.w * pos.w), a projective screen lookup)
                if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w)", a, comp);
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
                    char mb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));
                } else sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
            }
        } else if (addr == 0x3FC) {
            c->uses_front_facing = true;
            strcpy(e, "(front_facing ? 0xFFFFFFFFu : 0u)");
        } else if (addr == 0x2E0 || addr == 0x2E4) {
            c->uses_point_coord = true;
            sprintf(e, "as_type<uint>(point_coord[%d])", (addr >> 2) & 1);
        } else {
            sprintf(e, "0u /* attr %#x */", addr);
        }
    } else {
        sprintf(e, "0u /* attr %#x */", addr);
    }
    set_reg(c, dst, e);
}

/* AST in the geometry (current output vertex) and tess control (own control point, or the patch record with the P bit) stages. */
static void attr_store_staged(Ctx *c, int src, uint32_t addr) {
    char b[16], vb[40];
    const char *v = reg(c, src, b);
    int sv = c->ald_patch ? patch_slot(addr) : stage_slot(addr);
    if (sv < 0) {
        sp(&c->body, "    /* store attr %#x */\n", addr);
        return;
    }
    int comp = (addr >> 2) & 3;
    if (c->stage == STAGE_GEOMETRY) {
        c->gs_slots |= 1ull << sv;
        sp(&c->body, "    gso[%d][%d] = as_type<float>(%s);\n", sv, comp, v);
    } else if (c->ald_patch) {
        c->patch_slots |= 1ull << sv;
        sp(&c->body, "    tess_patch[pid * %uu + %d][%d] = as_type<float>(%s);\n", STAGE_SLOTS, sv, comp, v);
    } else {
        c->gs_slots |= 1ull << sv;
        (void)vb;
        sp(&c->body, "    tess_vtx[(pid * %uu + inv) * %uu + %d][%d] = as_type<float>(%s);\n", c->key ? c->key->tess_out_vertices : 1u, STAGE_SLOTS, sv, comp, v);
    }
}

static void attr_store(Ctx *c, int src, uint32_t addr) {
    char b[16];
    const char *v = reg(c, src, b);
    if (c->stage == STAGE_GEOMETRY || c->stage == STAGE_TESS_CTRL) {
        attr_store_staged(c, src, addr);
        return;
    }
    if (addr >= 0x60 && addr < 0x6C) {   // primitive id / layer / viewport index: kept for the stage-out record
        c->uses_vsys = true;
        sp(&c->body, "    vsys[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x70 && addr < 0x80) {
        sp(&c->body, "    out.pos[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << a;
        sp(&c->body, "    out.v%d[%d] = as_type<float>(%s);\n", a, (addr >> 2) & 3, v);
    } else if (addr == 0x6C) {
        c->uses_vsys = true;
        sp(&c->body, "    vsys[3] = as_type<float>(%s);\n", v);
        if (c->key && c->key->point_prim) {   // Metal rejects [[point_size]] in pipelines whose topology class is not point
            c->info->writes_point_size = true;
            sp(&c->body, "    out.psize = as_type<float>(%s);\n", v);
        }
    } else {
        sp(&c->body, "    /* store attr %#x */\n", addr);
    }
}


// ------------------------------------------------------------------ half precision (SM53)
// Swizzle: 0 H1_H0 (both halves), 1 F32 (one f32 used for both lanes), 2 H0_H0, 3 H1_H1.
// Merge: 0 H1_H0 (pack both), 1 F32 (lane 0 as f32), 2 MRG_H0 (replace low half), 3 MRG_H1 (replace high half).
static const char *hsrc(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    (void)c;
    char t[300];
    sprintf(t, "hx(%s, %du)", v, swz);
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS(v, sw, n, ab) hsrc(c, (v), (sw), (n), (ab), alloca(400))

static void hstore(Ctx *c, int d, const char *expr, int merge, bool sat) {
    char t[2048], e[1700];
    if (sat) sprintf(e, "saturate(%s)", expr);
    else strcpy(e, expr);
    if (d == RZ) return;
    c->reg_used[d] = 1;
    sprintf(t, "hpack(%s, %du, r%d)", e, merge, d);
    set_reg(c, d, t);
}

// immediate halves: 9 bits of each half's top (the low 6 mantissa bits are zero) plus separate signs
static uint32_t himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

static bool emit_half(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    char t[1800], ib[32], cb[256];
    uint16_t op = (uint16_t)(w >> 48);
    // ---- HADD2 / HMUL2 (reg 5d10/5d08, cbuf 7a8x/788x, imm 7a0x/780x, 32I 2c/2a)
    int kind = -1;   // 0 add, 1 mul
    int form = -1;   // 0 reg, 1 cbuf, 2 imm, 3 32i
    if ((op & 0xfff8) == 0x5d10) {
        kind = 0;
        form = 0;
    } else if ((op & 0xfff8) == 0x5d08) {
        kind = 1;
        form = 0;
    } else if ((op & 0xfe80) == 0x7a80) {
        kind = 0;
        form = 1;
    } else if ((op & 0xfe80) == 0x7a00) {
        kind = 0;
        form = 2;
    } else if ((op & 0xfe80) == 0x7880) {
        kind = 1;
        form = 1;
    } else if ((op & 0xfe80) == 0x7800) {
        kind = 1;
        form = 2;
    } else if ((op & 0xfe00) == 0x2c00) {
        kind = 0;
        form = 3;
    } else if ((op & 0xfe00) == 0x2a00) {
        kind = 1;
        form = 3;
    }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        const char *bv;
        if (form == 3) {
            merge = 0;
            sat = bits(w, 52, 1);
            swa = (int)bits(w, 53, 2);
            nega = kind == 0 ? bits(w, 56, 1) : false;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32));
            bv = ib;
        } else {
            // field layout per Ryujinx's InstDecoders (InstHadd2*/InstHmul2*)
            merge = (int)bits(w, 49, 2);
            swa = (int)bits(w, 47, 2);
            nega = bits(w, 43, 1);
            absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1);
                absb = bits(w, 30, 1);
                swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else {
                    nega = bits(w, 31, 1);
                    negb = false;
                }   // HMUL2 reg: the negate bit is A's at 31
                bv = R((int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1);
                negb = kind == 0 ? bits(w, 56, 1) : false;
                absb = bits(w, 54, 1);
                swb = 1;
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            } else {
                sat = bits(w, 52, 1);
                swb = 0;
                sprintf(ib, "0x%08xu", himm(w));
                bv = ib;
            }
        }
        sprintf(t, kind == 0 ? "(%s + %s)" : "(%s * %s)", HS(R(a), swa, nega, absa), HS(bv, swb, negb, absb));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HFMA2 (reg 5d00, cr 70xx|0x80, rc 60xx|0x80, imm 70xx, 32I 28)
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;   // b = cbuf, c = reg
    else if ((op & 0xf880) == 0x6080) form = 2;   // b = reg, c = cbuf
    else if ((op & 0xf880) == 0x7000) form = 3;   // b = imm, c = reg
    else if ((op & 0xfe00) == 0x2800) form = 4;   // 32I: c = d
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        const char *bv, *cv;
        char cb2[256];
        switch (form) {   // field layout per Ryujinx's InstDecoders (InstHfma2*); NegA is applied to B (same product)
            case 0:
                merge = (int)bits(w, 49, 2);
                sat = bits(w, 32, 1);
                negb = bits(w, 31, 1);
                negc = bits(w, 30, 1);
                swb = (int)bits(w, 28, 2);
                swc = (int)bits(w, 35, 2);
                bv = R((int)bits(w, 20, 8));
                cv = R((int)bits(w, 39, 8));
                break;
            case 1:   // b = cbuf, c = reg
                merge = (int)bits(w, 49, 2);
                sat = bits(w, 52, 1);
                negb = bits(w, 56, 1);
                negc = bits(w, 51, 1);
                swb = 1;
                swc = (int)bits(w, 53, 2);
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
                cv = R((int)bits(w, 39, 8));
                break;
            case 2:   // b = reg (field at 39), c = cbuf
                merge = (int)bits(w, 49, 2);
                sat = bits(w, 52, 1);
                negb = bits(w, 56, 1);
                negc = bits(w, 51, 1);
                swb = (int)bits(w, 53, 2);
                swc = 1;
                bv = R((int)bits(w, 39, 8));
                cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb2);
                break;
            case 3:   // b = imm, c = reg
                merge = (int)bits(w, 49, 2);
                sat = bits(w, 52, 1);
                negb = false;
                negc = bits(w, 51, 1);
                swb = 0;
                swc = (int)bits(w, 53, 2);
                sprintf(ib, "0x%08xu", himm(w));
                bv = ib;
                cv = R((int)bits(w, 39, 8));
                break;
            default:   // 32I: c = d
                merge = 0;
                sat = false;
                negb = false;
                negc = bits(w, 51, 1);
                swa = (int)bits(w, 47, 2);
                swb = 0;
                swc = 0;
                sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32));
                bv = ib;
                cv = R(d);
                break;
        }
        sprintf(t, "fma(%s, %s, %s)", HS(R(a), swa, false, false), HS(bv, swb, negb, false), HS(cv, swc, negc, false));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HSETP2 / HSET2 (reg forms)
    // forms: reg (5d20 / 5d18), immediate (7e00 / 7c00), constant buffer (7e80 / 7c80); the immediate and cbuf forms move cond to bit 49 and the
    // AND-both / BF flag to bit 53, negate/abs of B to bits 56/54 (layout checked against the LAS bezel_pbr shaders: cond LE, B = 1.0 in both halves)
    int hform = -1;
    bool setp;
    if ((op & 0xfff8) == 0x5d20) {
        hform = 0;
        setp = true;
    } else if ((op & 0xfff8) == 0x5d18) {
        hform = 0;
        setp = false;
    } else if ((op & 0xff80) == 0x7e00) {
        hform = 1;
        setp = true;
    } else if ((op & 0xff80) == 0x7c00) {
        hform = 1;
        setp = false;
    } else if ((op & 0xff80) == 0x7e80) {
        hform = 2;
        setp = true;
    } else if ((op & 0xff80) == 0x7c80) {
        hform = 2;
        setp = false;
    }
    if (hform >= 0) {
        const char *x = HS(R(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1));
        const char *y;
        int cop, hand_bit;
        if (hform == 0) {
            y = HS(R((int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
            cop = (int)bits(w, 35, 4);
            hand_bit = 49;
        } else {
            cop = (int)bits(w, 49, 4);
            hand_bit = 53;
            if (hform == 1) {
                sprintf(ib, "0x%08xu", himm(w));
                y = HS(ib, 0, bits(w, 56, 1), bits(w, 54, 1));
            } else y = HS(cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb), 1, bits(w, 56, 1), bits(w, 54, 1));
        }
        int bop = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        char lx[512], ly[512];
        sp(&c->body, "    { float2 hx_ = %s, hy_ = %s; bool pp = %s;\n", x, y, pp);
        fcmp_expr(lx, cop, "hx_.x", "hy_.x");
        fcmp_expr(ly, cop, "hx_.y", "hy_.y");
        sp(&c->body, "      bool l = %s, h = %s;\n", lx, ly);
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            bool hand = bits(w, hand_bit, 1);
            if (hand) sp(&c->body, "      l = l && h; h = !l;\n");
            if (pa != PT) {
                c->pred_used[pa] = 1;
                sp(&c->body, "      p%d = l %s pp;\n", pa, bool_op(bop));
            }
            if (pb != PT) {
                c->pred_used[pb] = 1;
                sp(&c->body, "      p%d = h %s pp;\n", pb, bool_op(bop));
            }
        } else {
            bool bf = bits(w, hand_bit, 1);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                sp(&c->body, bf ? "      r%d = (l ? 0x3c00u : 0u) | (h ? 0x3c000000u : 0u);\n" : "      r%d = (l ? 0xFFFFu : 0u) | (h ? 0xFFFF0000u : 0u);\n",
                   d);
            }
        }
        sp(&c->body, "    }\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures
// A bindless handle read from constant buffer `cb` (c2 = the driver's texture table, >= 3 = a user uniform buffer) at byte `off`: c2 handles map to
// the ordinary slot, user-buffer handles get a pseudo index (8192 + n) and are resolved from the bound buffer's contents at draw time.
static uint32_t bindless_index(Ctx *c, int cb, int off) {
    if (cb == 2) return (uint32_t)off / 4;
    for (int i = 0; i < c->bl_n; i++)
        if (c->bl_cb[i] == cb && c->bl_off[i] == off) return 8192u + (uint32_t)i;
    if (c->bl_n >= 64) {
        fail(c, "too many bindless texture handles");
        return 8192u;
    }
    c->bl_cb[c->bl_n] = cb;
    c->bl_off[c->bl_n] = off;
    return 8192u + (uint32_t)c->bl_n++;
}
static int tex_slot(Ctx *c, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) {
        int n = (int)(index - 8192);
        if (n >= c->bl_n) return 0;
        if (c->bl_slot[n]) {
            int s = c->bl_slot[n] - 1;
            if (type >= 0 && c->tex_weak[s]) {
                ShaderTexture *t = &c->info->tex[s];
                t->type = (uint8_t)type;
                t->shadow = shadow;
                t->is_array = array;
                c->tex_weak[s] = false;
            }
            return s;
        }
        bool weak = type < 0;
        int s = c->info->ntex++;
        if (s >= 63) {
            fail(c, "too many textures");
            return 0;
        }
        c->bl_slot[n] = s + 1;
        c->tex_weak[s] = weak;
        ShaderTexture *t = &c->info->tex[s];
        t->bindless = true;
        t->bindless_cbuf = (uint32_t)c->bl_cb[n];
        t->bindless_offset = (uint32_t)c->bl_off[n];
        t->type = (uint8_t)(weak ? TEXTYPE_2D : type);
        t->shadow = shadow;
        t->is_array = array;
        return s;
    }
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (type >= 0 && c->tex_weak[s]) {
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->shadow = shadow;
            t->is_array = array;
            c->tex_weak[s] = false;
        }
        return s;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int s = c->info->ntex++;
    if (s >= 63) {
        fail(c, "too many textures");
        return 0;
    }
    c->tex_slot[index] = s + 1;
    c->tex_weak[s] = weak;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    if (c->key && index * 4 >= 0x20 && index * 4 < 0x120 && ((c->key->depth_slots >> ((index * 4 - 0x20) / 8)) & 1)) t->depth = true;
    if (c->key && index * 4 >= 0x20 && index * 4 < 0x120 && ((c->key->stencil_slots >> ((index * 4 - 0x20) / 8)) & 1)) t->stencil = true, t->depth = false;
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    return s;
}

static const char *tex_type_name(int t) {
    switch (t) {
        case TEXTYPE_1D: return "1d";
        case TEXTYPE_3D: return "3d";
        case TEXTYPE_CUBE: return "cube";
        case TEXTYPE_2D_ARRAY: return "2d_array";
        case TEXTYPE_CUBE_ARRAY: return "cube_array";
        case TEXTYPE_1D_ARRAY: return "1d_array";
        case TEXTYPE_2D_MS: return "2d_ms";
        case TEXTYPE_2D_MS_ARRAY: return "2d_ms_array";
        default: return "2d";
    }
}

// Write texture results: component mask selects which of r,g,b,a go to consecutive registers.
static void write_tex_result(Ctx *c, int slot, const char *call, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    if (f16 && c->info->tex[slot].stencil) {
        fail(c, "half-precision (.F16) result from a stencil texture binding (the value is an integer)");
        return;
    }
    if (c->info->tex[slot].depth && !c->info->tex[slot].shadow && !strstr(call, ".gather("))
        sp(&c->body, "    { float4 t = float4(%s, 0.0f, 0.0f, 1.0f);\n", call);   // a depth texture sampled as colour: (d, 0, 0, 1)
    else sp(&c->body, "    { float4 t = %s;\n", call);
    if (f16) {
        // TEXS/TLDS .F16: components (in mask order) packed as half pairs into d0 (lo, hi) and d1 (lo, hi)
        int comps[4], n = 0;
        for (int comp = 0; comp < 4; comp++)
            if (mask & (1u << comp)) comps[n++] = comp;
        char v[4][16];
        for (int i = 0; i < 4; i++) {
            if (i < n) sprintf(v[i], "t[%d]", comps[i]);
            else strcpy(v[i], "0.0f");
        }
        const char *pk = "      r%d = as_type<uint>(half2(half(%s), half(%s)));\n";
        if (d0 != RZ) {
            c->reg_used[d0] = 1;
            sp(&c->body, pk, d0, v[0], v[1]);
        }
        if (d1 != RZ) {
            c->reg_used[d1] = 1;
            sp(&c->body, pk, d1, v[2], v[3]);
        }
        sp(&c->body, "    }\n");
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        // TEXS: up to two components in d0/d0+1, two more in d1/d1+1
        int out[4] = {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1};
        for (int i = 0; i < 4; i++) regs[i] = out[i];
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        int r = regs[n++];
        if (r != RZ) {
            c->reg_used[r] = 1;
            sp(&c->body, "      r%d = as_type<uint>(t[%d]);\n", r, comp);
        }
    }
    sp(&c->body, "    }\n");
}

// Integer texel coordinates in guest units -> the (possibly larger) allocation of texture slot `slot`.
static const char *RSC2(Ctx *c, int slot, const char *fmt, ...) {
    static char bufs[4][1024];
    static int bi;
    char *o = bufs[bi++ & 3];
    char inner[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inner, sizeof inner, fmt, ap);
    va_end(ap);
    if (c->stage == STAGE_VERTEX) {
        snprintf(o, 1024, "%s", inner);
        return o;
    }
    c->uses_rscale = true;
    snprintf(o, 1024, "rsc(%s, rscale[%d])", inner, slot + 1);
    return o;
}

// TEXS / TLDS: texture_info selects target and coordinate layout
static const char *alayer(Ctx *c, const char *r, int slot, char *buf) {
    (void)c;
    sprintf(buf, "min(%s, t%d.get_array_size() - 1u)", r, slot);
    return buf;
}
#define ALAYER(r, slot) alayer(c, (r), (slot), alloca(96))

// co: coordinate expressions (1 for 1D, 2 for 2D, 3 for 3D / cube), layer: uint expression or NULL, dref: depth
// reference or NULL, lod_kind 0 implicit / 1 level / 2 bias (lod), off: ivec2 texel offset or NULL.
static void emit_texs(Ctx *c, uint64_t w, bool fetch) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int msel = (int)bits(w, 50, 3);
    uint32_t mask;
    if (d1 == RZ) {
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12};
        mask = m2[msel];
    } else {
        static const uint32_t m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        mask = m4[msel];
    }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb), *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    char call[1024];
    int slot;
    if (!fetch) {
        switch (info) {
            case 0:
                slot = tex_slot(c, index, TEXTYPE_1D, false, false);
                sprintf(call, "t%d.sample(s%d, %s)", slot, slot, F(A));
                break;
            case 1:
                slot = tex_slot(c, index, TEXTYPE_2D, false, false);
                sprintf(call, "t%d.sample(s%d, float2(%s, %s))", slot, slot, F(A), F(Bv));
                break;
            case 2:
                slot = tex_slot(c, index, TEXTYPE_2D, false, false);
                sprintf(call, "t%d.sample(s%d, float2(%s, %s), level(0))", slot, slot, F(A), F(Bv));
                break;
            case 3:
                slot = tex_slot(c, index, TEXTYPE_2D, false, false);
                sprintf(call, "t%d.sample(s%d, float2(%s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv));
                break;
            case 4:
                slot = tex_slot(c, index, TEXTYPE_2D, true, false);
                sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s))", slot, slot, F(A), F(A1), F(Bv));
                break;
            case 5:
                slot = tex_slot(c, index, TEXTYPE_2D, true, false);
                sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, level(%s)))", slot, slot, F(A), F(A1), F(B1), F(Bv));
                break;
            case 6:
                slot = tex_slot(c, index, TEXTYPE_2D, true, false);
                sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, level(0)))", slot, slot, F(A), F(A1), F(Bv));
                break;
            case 7:
                slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
                sprintf(call, "t%d.sample(s%d, float2(%s, %s), %s)", slot, slot, F(A1), F(Bv), ALAYER(A, slot));
                break;
            case 8:
                slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
                sprintf(call, "t%d.sample(s%d, float2(%s, %s), %s, level(0))", slot, slot, F(A1), F(Bv), ALAYER(A, slot));
                break;
            case 9:
                slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true);
                sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, %s, level(0)))", slot, slot, F(A1), F(Bv), A, F(B1));
                break;
            case 10:
                slot = tex_slot(c, index, TEXTYPE_3D, false, false);
                sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv));
                break;
            case 11:
                slot = tex_slot(c, index, TEXTYPE_3D, false, false);
                sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(0))", slot, slot, F(A), F(A1), F(Bv));
                break;
            case 12:
                slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
                sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv));
                break;
            case 13:
                slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
                sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv), F(B1));
                break;
            default: fail(c, "texs info %d", info); return;
        }
    } else {
        switch (info) {
            case 0:
                slot = tex_slot(c, index, TEXTYPE_1D, false, false);
                sprintf(call, "t%d.read(uint(%s))", slot, A);
                break;
            case 1:
                slot = tex_slot(c, index, TEXTYPE_1D, false, false);
                sprintf(call, "t%d.read(uint(%s), uint(%s))", slot, A, Bv);
                break;
            case 2:
                slot = tex_slot(c, index, TEXTYPE_2D, false, false);
                sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv));
                break;
            case 4:
                slot = tex_slot(c, index, TEXTYPE_2D, false, false);
                sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv));
                break;
            case 5:
                slot = tex_slot(c, index, TEXTYPE_2D, false, false);
                sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv);
                break;
            case 7:
                slot = tex_slot(c, index, TEXTYPE_3D, false, false);
                sprintf(call, "t%d.read(uint3(%s, %s, %s))", slot, A, A1, Bv);
                break;
            case 8:
                slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
                sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A);
                break;
            case 12:
                slot = tex_slot(c, index, TEXTYPE_2D, false, false);
                sprintf(call, "t%d.read(%s, uint(%s))", slot,
                        RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv);
                break;
            default: fail(c, "tlds info %d", info); return;
        }
    }
    write_tex_result(c, slot, call, mask, d0, d1, true, !bits(w, 59, 1));   // 0xd0/0xd2: .F16 results
}

// TEX (full form): coordinates from consecutive registers at gpr8, extra parameters at gpr20.
static void emit_tex(Ctx *c, uint64_t w, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    if (bindless) {
        int v = b == RZ ? -1 : c->reg_cb[b];
        if (v < 0) {
            fail(c, "bindless texture handle is not a constant buffer load");
            return;
        }
        index = bindless_index(c, v >> 24, v & 0xffffff);
    }
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bindless ? bits(w, 36, 1) : bits(w, 54, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int lod_mode = bindless ? (int)bits(w, 37, 3) : (int)bits(w, 55, 3);   // 0 none, 1 lz, 2 lb, 3 ll, 6 lba, 7 lla
    int type = target == 0   ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
               : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
               : target == 2 ? TEXTYPE_3D
                             : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, dc, array);
    char coords[256] = "", layer[64] = "", tmp[64];
    int ar = a;
    // the hardware clamps the array index to the layer count
    if (array) {
        sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot);
        c->reg_used[a] = 1;
        ar = a + 1;
    }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    int pr = bindless ? b + 1 : b;   // bindless: Rb holds the handle, the extra parameters follow it
    char lod[96] = "", dref[64] = "", off[128] = "";
    if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
        c->reg_used[pr] = 1;
        sprintf(lod, lod_mode == 2 || lod_mode == 6 ? ", bias(as_type<float>(r%d))" : ", level(as_type<float>(r%d))", pr);
        pr++;
    } else if (lod_mode == 1) {
        strcpy(lod, ", level(0)");
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        sprintf(off, ", int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", pr, pr);
        pr++;
    }
    if (dc) {
        c->reg_used[pr] = 1;
        sprintf(dref, "as_type<float>(r%d)", pr);
        pr++;
    }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    char call[1024];
    if (dc) sprintf(call, "float4(t%d.sample_compare(s%d, %s(%s)%s%s, %s%s%s))", slot, slot, vec, coords, array ? ", " : "", layer, dref, lod, off);
    else sprintf(call, "t%d.sample(s%d, %s(%s)%s%s%s%s)", slot, slot, vec, coords, array ? ", " : "", layer, lod, off);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// TLD4 / TLD4.B (texture gather) and TMML / TMML.B (level of detail query): full forms with consecutive coordinate registers at Ra.
static void emit_tld4_tmml(Ctx *c, uint64_t w, bool bindless, bool tmml) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    if (bindless) {
        int v = b == RZ ? -1 : c->reg_cb[b];
        if (v < 0) {
            fail(c, "bindless texture handle is not a constant buffer load");
            return;
        }
        index = bindless_index(c, v >> 24, v & 0xffffff);
    }
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = !tmml && bits(w, 50, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int comp = tmml ? 0 : (int)bits(w, 56, 2);
    int aoff = tmml ? 0 : (int)bits(w, 54, 2);
    if (aoff == 2) {
        fail(c, "texture gather with per-texel offsets (PTP: Metal needs constant offsets)");
        return;
    }
    if (aoff == 3) {
        fail(c, "texture gather with reserved offset mode");
        return;
    }
    int type = target == 0   ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
               : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
               : target == 2 ? TEXTYPE_3D
                             : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    if (!tmml && (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY || type == TEXTYPE_3D)) {
        fail(c, "texture gather of target %d", target);
        return;
    }
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, dc, array);
    char coords[256] = "", layer[96] = "", tmp[64];
    int ar = a;
    if (array) {
        sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot);
        c->reg_used[a] = 1;
        ar = a + 1;
    }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    int pr = bindless ? b + 1 : b;
    char call[1024];
    static const char *const cn[] = {"x", "y", "z", "w"};
    char cexpr[900];
    sprintf(cexpr, "%s(%s)", vec, coords);
    if (aoff == 1 && !tmml) {
        /* AOFFI: a per-thread texel offset in Rpr (4-bit signed x, y). Metal only takes constant gather offsets, but a whole-texel offset keeps the
         * fractional position, so gathering at coord + offset / size selects exactly the shifted 2x2 footprint (the sampler's wrap/clamp still applies) */
        if (ncoord != 2) {
            fail(c, "texture gather offset on target %d", target);
            return;
        }
        c->reg_used[pr] = 1;
        char gs[200];
        if (c->stage == STAGE_VERTEX) sprintf(gs, "float2(t%d.get_width(), t%d.get_height())", slot, slot);
        else {
            c->uses_rscale = true;
            sprintf(gs, "(float2(t%d.get_width(), t%d.get_height()) / rscale[%d].xy)", slot, slot, slot + 1);
        }
        sprintf(cexpr, "float2(%s) + float2(int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)) / %s", coords, pr, pr, gs);
        pr++;
    }
    if (tmml) {
        // fixed point result: 8 fractional bits, x = clamped level, y = unclamped lambda (the guest converts with I2F)
        sprintf(call, "int2(float2(t%d.calculate_clamped_lod(s%d, %s(%s)), t%d.calculate_unclamped_lod(s%d, %s(%s))) * 256.0f)", slot, slot, vec, coords, slot,
                slot, vec, coords);
        int n = 0;
        sp(&c->body, "    { int2 q = %s;\n", call);
        for (int i = 0; i < 2; i++)
            if (mask & (1u << i)) {
                int r = d == RZ ? RZ : d + n;
                n++;
                if (r != RZ) {
                    c->reg_used[r] = 1;
                    c->reg_cb[r] = -1;
                    sp(&c->body, "      r%d = uint(q[%d]);\n", r, i);
                }
            }
        sp(&c->body, "    }\n");
        return;
    }
    if (dc) {
        c->reg_used[pr] = 1;
        sprintf(call, "float4(t%d.gather_compare(s%d, %s%s%s, as_type<float>(r%d)))", slot, slot, cexpr, array ? ", " : "", layer, pr);
    } else if (c->info->tex[slot].depth || c->info->tex[slot].shadow)
        sprintf(call, "float4(t%d.gather(s%d, %s%s%s))", slot, slot, cexpr, array ? ", " : "", layer);   // depth textures gather the depth only
    else sprintf(call, "t%d.gather(s%d, %s%s%s, int2(0), component::%s)", slot, slot, cexpr, array ? ", " : "", layer, cn[comp]);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// TLD4S: short gather form (2D); coordinates like TEXS (x = Ra, y = Rb, or x = Ra, y = Ra+1 with the depth reference in Rb); four results in two register pairs.
// With the offset flag: x = Ra, y = Ra+1 and Rb holds the packed texel offset (see emit_tld4_tmml for how a runtime offset is applied).
static void emit_tld4s(Ctx *c, uint64_t w) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    bool dc = bits(w, 50, 1), aoffi = bits(w, 51, 1), f16 = bits(w, 55, 1);
    int comp = (int)bits(w, 52, 2);
    if (aoffi && dc) {
        fail(c, "tld4s with both a depth reference and offsets");
        return;
    }
    char ra[16], ra1[16], rb[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb);
    int slot = tex_slot(c, index, TEXTYPE_2D, dc, false);
    static const char *const cn[] = {"x", "y", "z", "w"};
    char call[900], cxy[600];
    if (aoffi) {
        char gs[200];
        if (c->stage == STAGE_VERTEX) sprintf(gs, "float2(t%d.get_width(), t%d.get_height())", slot, slot);
        else {
            c->uses_rscale = true;
            sprintf(gs, "(float2(t%d.get_width(), t%d.get_height()) / rscale[%d].xy)", slot, slot, slot + 1);
        }
        sprintf(cxy, "float2(%s, %s) + float2(int2(int(%s << 28) >> 28, int(%s << 24) >> 28)) / %s", F(A), F(A1), Bv, Bv, gs);
    } else if (dc) sprintf(cxy, "float2(%s, %s)", F(A), F(A1));
    else sprintf(cxy, "float2(%s, %s)", F(A), F(Bv));
    if (dc) sprintf(call, "float4(t%d.gather_compare(s%d, %s, %s))", slot, slot, cxy, F(Bv));
    else if (c->info->tex[slot].depth || c->info->tex[slot].shadow) sprintf(call, "float4(t%d.gather(s%d, %s))", slot, slot, cxy);
    else sprintf(call, "t%d.gather(s%d, %s, int2(0), component::%s)", slot, slot, cxy, cn[comp]);
    write_tex_result(c, slot, call, 0xf, d0, d1, true, f16);
}

// TLD: integer texel fetch, full form (the TEX operand layout): Ra = [array layer,] integer coordinates; Rb = [lod] [packed texel offset].
// Layout decoded from the Maxwell tables (target 28-30, mask 31-34, offset flag 35, index 36-48, multisample 49/50, lod flag 55); unverified against real shaders.
static void emit_tld(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), aoffi = bits(w, 35, 1), ms = bits(w, 50, 1), has_lod = bits(w, 55, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    if (ms && target != 1) {
        fail(c, "tld.ms of a texture that is not 2D");
        return;
    }
    if (target == 3) {
        fail(c, "tld of a cube texture");
        return;
    }
    /* TLD.MS: the binding is a multisampled 2D texture (texture2d_ms, read by sample index; the only Maxwell access to one); the sample index is the Rb
     * word after the lod (nouveau's operand order: lod, multisample index, offsets) and texture2d_ms has no levels */
    int type = ms ? (array ? TEXTYPE_2D_MS_ARRAY : TEXTYPE_2D_MS) : target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D) : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D) : TEXTYPE_3D;
    if (type == TEXTYPE_3D) array = false;
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, false, array);
    int ar = a, pr = b;
    char layer[96] = "", off[3][48] = {"", "", ""}, lv[48] = "0u";
    if (array) {
        sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot);
        c->reg_used[a] = 1;
        ar = a + 1;
    }
    if (has_lod) {
        c->reg_used[pr] = 1;
        sprintf(lv, "uint(r%d)", pr);
        pr++;
    }
    if (ms) {
        c->reg_used[pr] = 1;
        sprintf(lv, "min(uint(r%d), t%d.get_num_samples() - 1u)", pr, slot);
        pr++;
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        for (int i = 0; i < ncoord; i++) sprintf(off[i], " + (int(r%d << %d) >> 28)", pr, 28 - 4 * i); /* 4-bit signed fields: x, y, z */
        pr++;
    }
    char x[3][96];
    for (int i = 0; i < ncoord; i++) {
        c->reg_used[ar + i] = 1;
        sprintf(x[i], "uint(int(r%d)%s)", ar + i, off[i]);
    }
    char call[900];
    const char *comma = array ? ", " : "";
    /* Metal 1D textures have a single level and their read() only takes a constant 0 lod, so the lod register is consumed but not passed. */
    if (ncoord == 1) sprintf(call, "t%d.read(%s%s%s)", slot, x[0], comma, layer);
    else if (ncoord == 2) sprintf(call, "t%d.read(%s%s%s, %s)", slot, RSC2(c, slot, "uint2(%s, %s)", x[0], x[1]), comma, layer, lv);
    else sprintf(call, "t%d.read(uint3(%s, %s, %s), %s)", slot, x[0], x[1], x[2], lv);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// TXD: sample with explicit derivatives. Ra = [array layer,] coordinates; Rb = d/dx components then d/dy components. Not available for 1D (Metal has no 1D gradients).
// Layout decoded from the Maxwell tables, unverified against real shaders.
static void emit_txd(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    if (bits(w, 35, 1)) {
        fail(c, "txd with offsets");
        return;
    }
    if (target == 0) {
        fail(c, "txd of a 1D texture (Metal has no 1D gradients)");
        return;
    }
    int type = target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D) : target == 2 ? TEXTYPE_3D : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    if (type == TEXTYPE_3D) array = false;
    int ncoord = target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, false, array);
    char coords[256] = "", dx[200] = "", dy[200] = "", layer[96] = "", tmp[64];
    int ar = a;
    if (array) {
        sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot);
        c->reg_used[a] = 1;
        ar = a + 1;
    }
    for (int i = 0; i < ncoord; i++) {
        c->reg_used[ar + i] = 1;
        c->reg_used[b + i] = 1;
        c->reg_used[b + ncoord + i] = 1;
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        strcat(coords, tmp);
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", b + i);
        strcat(dx, tmp);
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", b + ncoord + i);
        strcat(dy, tmp);
    }
    const char *vec = ncoord == 2 ? "float2" : "float3";
    const char *grad = type == TEXTYPE_3D ? "gradient3d" : (type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY) ? "gradientcube" : "gradient2d";
    char call[1024];
    sprintf(call, "t%d.sample(s%d, %s(%s)%s%s, %s(%s(%s), %s(%s)))", slot, slot, vec, coords, array ? ", " : "", layer, grad, vec, dx, vec, dy);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// ------------------------------------------------------------------ video ALU (VADD, VMAD, VABSDIFF, VSHL, VSHR, VSET, VSETP, VMNMX)
// One operand scheme for the whole family (layout facts: envytools envydis gm107, MIT): A = Ra viewed through the selector at bits 36-38 (0-3 byte, 4 low half,
// 5 high half, 6-7 word) with sign bit 48; B = register Rb (bits 20-27) through the selector at bits 28-30 when bit 50 is set, else the 16-bit immediate at 20,
// both signed by bit 49; Rc at bits 39-46; secondary op at 51-53; dest signed at bit 54; saturate at bit 55. Operands are widened to 64-bit `long`.
static void video_sel(char *out, const char *src, int sel, bool sg) {
    static const int sh[] = {0, 8, 16, 24, 0, 16, 0, 0};
    static const char *const ux[] = {"0xFFu", "0xFFu", "0xFFu", "0xFFu", "0xFFFFu", "0xFFFFu", "0xFFFFFFFFu", "0xFFFFFFFFu"};
    static const int nb[] = {8, 8, 8, 8, 16, 16, 32, 32};
    if (nb[sel] == 32) sprintf(out, sg ? "long(int(%s))" : "long(%s)", src);
    else if (sg) sprintf(out, "long(int(%s << %du) >> %d)", src, 32 - nb[sel] - sh[sel], 32 - nb[sel]);
    else sprintf(out, "long((%s >> %du) & %s)", src, sh[sel], ux[sel]);
}

static void video_ab(Ctx *c, uint64_t w, char *ea, char *eb) {
    char rb[16], ra[16];
    video_sel(ea, reg(c, (int)bits(w, 8, 8), ra), (int)bits(w, 36, 3), bits(w, 48, 1));
    bool bsg = bits(w, 49, 1);
    if (bits(w, 50, 1)) video_sel(eb, reg(c, (int)bits(w, 20, 8), rb), (int)bits(w, 28, 3), bsg);
    else {
        uint32_t im = (uint32_t)bits(w, 20, 16);
        sprintf(eb, "long(%d)", bsg ? (int)(int16_t)im : (int)im);
    }
}

// Secondary operation with Rc (bits 51-53): MRG_16H, MRG_16L, MRG_8B0, MRG_8B2 merge the result into Rc, ACC adds, MIN/MAX compare, 7 passes it through.
static void video_tail(Ctx *c, int vop, bool dfmt, int rc, int d) {
    char rcb[16];
    sp(&c->body, "      uint res = uint(vr); uint vc = %s;\n", reg(c, rc, rcb));
    switch (vop) {
        case 0: sp(&c->body, "      res = insert_bits(vc, res, 16u, 16u);\n"); break;
        case 1: sp(&c->body, "      res = insert_bits(vc, res, 0u, 16u);\n"); break;
        case 2: sp(&c->body, "      res = insert_bits(vc, res, 0u, 8u);\n"); break;
        case 3: sp(&c->body, "      res = insert_bits(vc, res, 16u, 8u);\n"); break;
        case 4: sp(&c->body, "      res = res + vc;\n"); break;
        case 5: sp(&c->body, dfmt ? "      res = uint(min(int(res), int(vc)));\n" : "      res = min(res, vc);\n"); break;
        case 6: sp(&c->body, dfmt ? "      res = uint(max(int(res), int(vc)));\n" : "      res = max(res, vc);\n"); break;
        default: break;
    }
    if (d != RZ) {
        c->reg_used[d] = 1;
        c->reg_cb[d] = -1;
        sp(&c->body, "      r%d = res;\n", d);
    }
    sp(&c->body, "    }\n");
}

// A video op that yields `vr` (the exact 64-bit result in terms of va, vb), then saturates/wraps it to 32 bits and applies the secondary op.
static void video_op(Ctx *c, uint64_t w, int d, const char *vr_expr, bool with_tail) {
    char ea[160], eb[160];
    video_ab(c, w, ea, eb);
    bool dfmt = bits(w, 54, 1), sat = bits(w, 55, 1);
    sp(&c->body, "    { long va = %s, vb = %s; long vr = %s;\n", ea, eb, vr_expr);
    if (sat) sp(&c->body, dfmt ? "      vr = clamp(vr, -2147483648l, 2147483647l);\n" : "      vr = clamp(vr, 0l, 4294967295l);\n");
    if (with_tail) video_tail(c, (int)bits(w, 51, 3), dfmt, (int)bits(w, 39, 8), d);
    else {
        sp(&c->body, "      uint res = uint(vr);\n");
        if (d != RZ) {
            c->reg_used[d] = 1;
            c->reg_cb[d] = -1;
            sp(&c->body, "      r%d = res;\n", d);
        }
        sp(&c->body, "    }\n");
    }
}

static const char *video_cmp(int op, char *buf) {
    static const char *const ops[] = {"false", "va < vb", "va == vb", "va <= vb", "va > vb", "va != vb", "va >= vb", "true"};
    strcpy(buf, ops[op & 7]);
    return buf;
}

// ------------------------------------------------------------------ instruction translation
#include "surface.inc"

static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {
    char t[2048], u[2048];
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint16_t op13 = (uint16_t)(w >> 51);   // top 13 bits
    // predicate guard. SSY/PBK/PCNT/PRET take no predicate: those bits are zero in the encoding,
    // which would otherwise read as @P0.
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull || top12 == 0xe270000000000000ull) {
        pr = PT;
        prn = false;
    }
    bool guarded = !(pr == PT && !prn);
    if (pr == PT && prn) return;   // @!pt: never executes
    if (guarded) sp(&c->body, "  if (%s) {\n", P(pr, prn));

#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {   // EXIT
        if (c->has_pexit) sp(&c->body, "    if (pex != 0xFFFFFFFFu) { pc = pex; pex = 0xFFFFFFFFu; continue; }\n");
        sp(&c->body, c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe330000000000000ull, 0xfff0000000000000ull)) {   // KIL
        c->info->uses_discard = true;
        sp(&c->body, "    discard_fragment();\n");
    } else if (OPM(0xe240000000000000ull, 0xfff0000000000020ull)) {   // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        c->uses_branches = true;
        sp(&c->body, "    pc = %uu; continue;\n", target);
    } else if (OPM(0xe290000000000000ull, 0xfff0000000000020ull)) {   // SSY
        c->uses_branches = true;
        sp(&c->body, "    ssy[ssy_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2a0000000000000ull, 0xfff0000000000020ull)) {   // PBK
        c->uses_branches = true;
        sp(&c->body, "    pbk[pbk_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2b0000000000000ull, 0xfff0000000000020ull)) {   // PCNT
        c->uses_branches = true;
        sp(&c->body, "    pcnt[pcnt_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull)) {   // SYNC
        sp(&c->body, "    pc = ssy[--ssy_n]; continue;\n");
    } else if (OPM(0xe340000000000000ull, 0xfff0000000000000ull)) {   // BRK
        sp(&c->body, "    pc = pbk[--pbk_n]; continue;\n");
    } else if (OPM(0xe350000000000000ull, 0xfff0000000000000ull)) {   // CONT
        sp(&c->body, "    pc = pcnt[pcnt_n - 1]; continue;\n");
    } else if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xef98000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR
    } else if (OPM(0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(0xe000004000000000ull, 0xff00004000000000ull)) {
        // IPA: interpolate fragment input
        bool idx = bits(w, 38, 1);
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);   // 0 pass, 1 multiply, 2 constant, 3 sc
        if (idx) {
            c->ipa_mode = mode;
            c->ipa_mul = (int)bits(w, 20, 8);
            char rb[16];
            attr_load(c, d, 0, reg(c, a, rb));
            c->ipa_mode = -1;
            if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
            goto ipa_done;
        }
        if (addr >= 0x80 && addr < 0x280) {
            int at = (addr - 0x80) >> 4;
            if (mode == 2) c->flat_mask |= 1u << at;
        }
        c->ipa_mode = mode;
        c->ipa_mul = (int)bits(w, 20, 8);
        attr_load(c, d, addr, NULL);
        c->ipa_mode = -1;
        if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
    ipa_done:;
    } else if (OPM(0xefa0000000000000ull, 0xfff8000000000000ull)) {   // AL2P: physical attribute address = Ra + imm
        char t2[64];
        sprintf(t2, "(%s + 0x%xu)", R(a), (uint32_t)bits(w, 20, 11));
        set_reg(c, d, t2);
    } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull)) {   // LD attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        int ia = (int)bits(w, 8, 8);
        c->ald_vertex = (int)bits(w, 39, 8);   // geometry / tessellation: the vertex index register
        c->ald_patch = bits(w, 31, 1);
        c->ald_out = bits(w, 32, 1);
        bool staged = c->stage == STAGE_GEOMETRY || c->stage == STAGE_TESS_CTRL || c->stage == STAGE_TESS_EVAL;
        c->ald_vlatched = staged && c->ald_vertex != RZ;
        if (c->ald_vlatched) sp(&c->body, "    { uint vx = %s;\n", R(c->ald_vertex));
        if (ia != RZ) {
            /* a[Ra + imm]: the index is latched first, the destination may overlap Ra */
            sp(&c->body, "    { uint ix = %s;\n", R(ia));
            for (int i = 0; i < n; i++) {
                char ib[48];
                sprintf(ib, "(ix + %uu)", addr + 4 * i);
                attr_load(c, d + i, 0, ib);
            }
            sp(&c->body, "    }\n");
        } else
            for (int i = 0; i < n; i++) attr_load(c, d + i, addr + 4 * i, NULL);
        if (c->ald_vlatched) sp(&c->body, "    }\n");
        c->ald_vlatched = false;
    } else if (OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {   // ST attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        c->ald_vertex = RZ;
        c->ald_patch = bits(w, 31, 1);
        c->ald_out = false;
        for (int i = 0; i < n; i++) attr_store(c, d + i, addr + 4 * i);
    } else if (OPM(0xef90000000000000ull, 0xfff8000000000000ull)) {   // LD c[idx][reg + imm]
        int idx = (int)bits(w, 36, 5);
        int64_t off = (int64_t)(int16_t)bits(w, 20, 16);   // signed 16 bits (yuzu ldc.offset s64; nvdisasm prints c[6][R19-0x10]): Animal Crossing's skinning reads the
                                                            // bone matrix at R19 - 0x10; as 0xfff0 that is past every buffer, cbr() gave 0 and the characters collapsed
        int sz = (int)bits(w, 48, 3);
        int n = sz == 5 ? 2 : 1;
        char rb[16];
        const char *dyn = a == RZ ? NULL : reg(c, a, rb);
        /* each half is written as soon as it is loaded: when the address register is also a destination (LDC.64 R12, c[6][R12 + 0x18]), it is
         * written last, or the second half would be read at an address made from the first (SMO's skinning: spiky, torn character meshes) */
        int last = dyn && a >= d && a < d + n ? a - d : n - 1;
        for (int j = 0; j < n; j++) {
            int i = j == n - 1 ? last : (j >= last ? j + 1 : j);
            char cb[256];
            cbuf_ref(c, idx, off + 4 * i, dyn, cb);
            if (!dyn && (off + 4 * i) & 0) {}
            if (dyn) c->info->cbuf_size[idx] = 0x10000;
            set_reg(c, d + i, cb);
        }
        /* A texture handle loaded from the texture/sampler handle table (c2). With a register index the handle is only known at run time (an array of
         * textures selected per pixel, water_uber / bezel_pbr): bindless sampling then resolves to the table's first entry (offset + 0), a documented
         * approximation that keeps the draw alive instead of skipping it. */
        if (idx >= 2 && d != RZ && n == 1) {
            c->reg_cb[d] = (idx << 24) | (int)off;
            if (dyn) c->info->bindless_approx = true;
        }
    } else if (OPM(0xd000000000000000ull, 0xf600000000000000ull)) {   // TEXS
        emit_texs(c, w, false);
    } else if (OPM(0xd200000000000000ull, 0xf600000000000000ull)) {   // TLDS
        emit_texs(c, w, true);
    } else if (OPM(0xc038000000000000ull, 0xfc38000000000000ull)) {   // TEX
        emit_tex(c, w, false);
    } else if (OPM(0xdc38000000000000ull, 0xff38000000000000ull)) {   // TLD
        emit_tld(c, w);
    } else if (OPM(0xde38000000000000ull, 0xfff8000000000000ull)) {   // TXD
        emit_txd(c, w);
    } else if (OPM(0xdeb8000000000000ull, 0xfff8000000000000ull)) {   // TEX.B (bindless)
        emit_tex(c, w, true);
    } else if (OPM(0xc838000000000000ull, 0xfc38000000000000ull)) {   // TLD4
        emit_tld4_tmml(c, w, false, false);
    } else if (OPM(0xdef8000000000000ull, 0xfff8000000000000ull)) {   // TLD4.B
        emit_tld4_tmml(c, w, true, false);
    } else if (OPM(0xdf58000000000000ull, 0xfff8000000000000ull)) {   // TMML
        emit_tld4_tmml(c, w, false, true);
    } else if (OPM(0xdf60000000000000ull, 0xfff8000000000000ull)) {   // TMML.B
        emit_tld4_tmml(c, w, true, true);
    } else if (OPM(0xdf00000000000000ull, 0xff40000000000000ull)) {   // TLD4S
        emit_tld4s(c, w);
    } else if (OPM(0xdf48000000000000ull, 0xfff8000000000000ull) || OPM(0xdf40000000000000ull, 0xfff8000000000000ull)) {   // TXQ, TXA
        /* TXA (texture attribute query) is read like TXQ: same index / selector / mask fields, and the attributes Metal can answer are the ones TXQ names
         * (selector 1: width, height, depth or layers, mip count). Other selectors are refused by name. */
        const char *qn = (w >> 52 & 0xf) == 0x4 ? "txa" : "txq";
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int qt = (int)bits(w, 22, 6);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int slot = tex_slot(c, index, -1, false, false);   // the type comes from a sample, if any
        if (qt == 1) {                                     // dimension
            if (c->stage != STAGE_VERTEX) {                // report the guest size of scaled allocations
                c->uses_rscale = true;
                sp(&c->body,
                   "    { uint lv = %s; uint4 q = uint4(uint(round(float(t%d.get_width(lv)) / rscale[%d].x)), "
                   "uint(round(float(t%d.get_height(lv)) / rscale[%d].y)), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot + 1, slot, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(t%d.get_width(lv), t%d.get_height(lv), 1, t%d.get_num_mip_levels());\n", R(a), slot, slot,
                   slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) {
                    c->reg_used[d + n] = 1;
                    sp(&c->body, "      r%d = q[%d];\n", d + n, i);
                    n++;
                }
            sp(&c->body, "    }\n");
        } else {
            fail(c, "%s type %d", qn, qt);
        }
    } else if (OPM(0x5080000000000000ull, 0xfff8000000000000ull)) {   // MUFU
        int fn = (int)bits(w, 20, 4);
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1));
        switch (fn) {
            case 0: sprintf(t, "cos(%s)", x); break;
            case 1: sprintf(t, "sin(%s)", x); break;
            case 2: sprintf(t, "exp2(%s)", x); break;
            case 3: sprintf(t, "log2(%s)", x); break;
            case 4: sprintf(t, "(1.0f / %s)", x); break;
            case 5: sprintf(t, "rsqrt(%s)", x); break;
            case 8: sprintf(t, "sqrt(%s)", x); break;
            default: fail(c, "mufu %d", fn); return;
        }
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3890000000000000ull, 0xfef8000000000000ull)) {   // RRO: range reduction (no-op for us)
        const char *x = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        set_reg(c, d, FRES(x, false));
    } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {   // FADD
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1)));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {   // FADD32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 56, 1), bits(w, 54, 1)), NA(u, bits(w, 53, 1), bits(w, 57, 1)));
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {   // FMUL
        static const char *const scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        sprintf(t, "(%s * %s%s)", F(R(a)), NA(F(B(true)), bits(w, 48, 1), false), scale[bits(w, 41, 3)]);
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {   // FMUL32I
        sprintf(t, "(%s * as_type<float>(0x%08xu))", F(R(a)), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {   // FFMA
        const char *bv;
        char cbuf_c[256], bbuf[256];
        const char *cv = operand_c_for_ffma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "fma(%s, %s, %s)", F(R(a)), NA(F(bv), bits(w, 48, 1), false), NA(F(cv), bits(w, 49, 1), false));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {   // FFMA32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "fma(%s, %s, %s)", NA(F(R(a)), bits(w, 56, 1), false), u, NA(F(R(d)), bits(w, 57, 1), false));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3860000000000000ull, 0xfef8000000000000ull)) {   // FMNMX
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        /* precise:: so the non-NaN operand wins (Maxwell FMNMX) whatever math mode the library is compiled with (NAPPLE_GFX_MATH, mtl_shader.m) */
        sprintf(t, "(%s ? precise::fmin(%s, %s) : precise::fmax(%s, %s))", P((int)bits(w, 39, 3), bits(w, 42, 1)), x, y, x, y);
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36b0000000000000ull, 0xfef0000000000000ull)) {   // FSETP
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(F(B(true)), bits(w, 6, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (p != PT) {
            c->pred_used[p] = 1;
            sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n      p%d = cmp %s pp;\n", cmp, pc_, p, bool_op(bo));
        } else sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
        if (q != PT) {
            c->pred_used[q] = 1;
            sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo));
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5800000000000000ull, 0xff00000000000000ull) || OPM(0x4800000000000000ull, 0xff00000000000000ull) || /* 0x49 is DSET */
               OPM(0x3000000000000000ull, 0xfe00000000000000ull)) {                                                      // FSET
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 54, 1)), *y = NA(F(B(true)), bits(w, 53, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {   // FCMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, true, bb);
            cv = R((int)bits(w, 39, 8));
        }
        fcmp_expr(cmp, (int)bits(w, 48, 4), F(cv), "0.0f");
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {   // MOV
        set_reg(c, d, B(false));
        if (d != RZ && (w >> 60) == 0x4 && bits(w, 34, 5) >= 2)
            c->reg_cb[d] = (int)(bits(w, 34, 5) << 24) | (int)(bits(w, 20, 14) * 4);   // a texture handle loaded from c2 or a user uniform buffer
    } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {                    // MOV32I
        sprintf(t, "0x%08xu", (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a0000000000000ull, 0xfef8000000000000ull)) {   // SEL
        sprintf(t, "(%s ? %s : %s)", P((int)bits(w, 39, 3), bits(w, 42, 1)), R(a), B(false));
        set_reg(c, d, t);
    } else if ((OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
                OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) &&
               (bits(w, 10, 2) == 3 || bits(w, 8, 2) == 3)) {   // F2I from f64 and/or to a 64-bit integer
        int sfmt = (int)bits(w, 10, 2), dsz = (int)bits(w, 8, 2);
        char s64[700];
        if (sfmt == 3) strcpy(s64, DB());
        else if (sfmt == 1) sprintf(s64, "sd_from_f32(as_type<uint>(float(as_type<half2>(%s)[%d])))", B(false), (int)bits(w, 41, 1));
        else sprintf(s64, "sd_from_f32(%s)", B(true));
        use_f64(c);
        static const unsigned nb[] = {8, 16, 32, 64};
        sprintf(t, "sd_to_int(%s, %uu, %uu, %uu)", DNA(s64, bits(w, 45, 1), bits(w, 49, 1)), (unsigned)bits(w, 39, 2), bits(w, 12, 1) ? 1u : 0u, nb[dsz]);
        if (dsz == 3) dstore(c, d, t);
        else {
            sprintf(u, "uint(%s)", t);
            set_reg(c, d, u);
        }
    } else if ((OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
                OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) &&
               (bits(w, 10, 2) == 3 || bits(w, 8, 2) == 3)) {   // I2F to f64 and/or from a 64-bit integer
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2), dfm = (int)bits(w, 8, 2);
        unsigned rm = (unsigned)bits(w, 39, 2);
        char s64[700], src[300];
        const char *bv = B(false);
        if (sz == 3) strcpy(s64, DB());
        else {
            if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
            else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
            else sprintf(src, sgn ? "int(%s)" : "%s", bv);
            sprintf(s64, sgn ? "ulong(long(%s))" : "ulong(%s)", src);
        }
        use_f64(c);
        if (dfm == 3) {
            sprintf(t, "sd_from_int(%s, %uu, %uu)", s64, sgn ? 1u : 0u, rm);
            dstore(c, d, DNA(t, bits(w, 45, 1), bits(w, 49, 1)));
        } else {   // a 64-bit integer into f32 (or f16 through f32)
            sprintf(u, "as_type<float>(sd_int_to_f32(%s, %uu, %uu))", s64, sgn ? 1u : 0u, rm);
            if (dfm == 1) {
                snprintf(t, sizeof t, "(uint(as_type<ushort>(half(%s))))", NA(u, bits(w, 45, 1), bits(w, 49, 1)));
                set_reg(c, d, t);
            } else set_reg(c, d, FRES(NA(u, bits(w, 45, 1), bits(w, 49, 1)), false));
        }
    } else if ((OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
                OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) &&
               (bits(w, 10, 2) == 3 || bits(w, 8, 2) == 3)) {   // F2F with an f64 source and/or destination
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        int fm = (int)(bits(w, 39, 2) |
                       (bits(w, 42, 1) << 2));   // 4..7: round to an integral value (floor/ceil/trunc/rint); 0..3: the rounding of a narrowing result
        char s64[700], sv[512];
        use_f64(c);
        if (sfmt == 3) strcpy(s64, DNA(DB(), bits(w, 45, 1), bits(w, 49, 1)));
        else {   // f16/f32 source: modifiers and integer rounding in float, then the exact widening
            if (sfmt == 1) sprintf(sv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
            else strcpy(sv, F(B(true)));
            const char *xf = NA(sv, bits(w, 45, 1), bits(w, 49, 1));
            static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
            if (fm >= 4) sprintf(t, "%s(%s)", rf[fm - 4], xf);
            else strcpy(t, xf);
            sprintf(s64, "sd_from_f32(as_type<uint>(%s))", t);
        }
        if (sfmt == 3 && fm >= 4) {
            sprintf(u, "sd_rint(%s, %uu)", s64, (unsigned)(fm - 4));
            strcpy(s64, u);
        }
        if (dfmt == 3) dstore(c, d, s64);
        else {   // f64 -> f32 (a half goes through f32: two roundings, rare)
            sprintf(t, "as_type<float>(sd_to_f32(%s, %uu))", s64, fm >= 4 ? 0u : (unsigned)fm);
            if (dfmt == 1) {
                char h[2200];
                snprintf(h, sizeof h, "(uint(as_type<ushort>(half(%s))))", bits(w, 50, 1) ? (sprintf(sv, "saturate(%s)", t), sv) : t);
                set_reg(c, d, h);
            } else set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) {   // F2I (src fmt bits 10-11: 1 F16; dst size bits 8-9)
        char srcv[512];
        if (bits(w, 10, 2) == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int rnd = (int)bits(w, 39, 2);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        if (i16) sprintf(t, sgn ? "uint(int(clamp(%s(%s), -32768.0f, 32767.0f)))" : "uint(clamp(%s(%s), 0.0f, 65535.0f))", rf[rnd], x);
        else sprintf(t, sgn ? "uint(int(clamp(%s(%s), -2147483648.0f, 2147483520.0f)))" : "uint(clamp(%s(%s), 0.0f, 4294967040.0f))", rf[rnd], x);
        set_reg(c, d, t);
    } else if (OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) {   // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, sgn ? "int(%s)" : "%s", bv);
        sprintf(u, "float(%s)", src);
        if (bits(w, 8, 2) == 1) {   // F16 result in the low half
            char h[700];
            snprintf(h, sizeof h, "(uint(as_type<ushort>(half(%s))))", NA(u, bits(w, 45, 1), bits(w, 49, 1)));
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(NA(u, bits(w, 45, 1), bits(w, 49, 1)), false));
        }
    } else if (OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) {   // F2F (formats: 1 F16, 2 F32; bit 41 = high half)
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        char srcv[512];
        if (sfmt == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int mode = (int)((bits(w, 39, 2)) | (bits(w, 42, 1) << 2));
        switch (mode) {
            case 4: sprintf(t, "rint(%s)", x); break;
            case 5: sprintf(t, "floor(%s)", x); break;
            case 6: sprintf(t, "ceil(%s)", x); break;
            case 7: sprintf(t, "trunc(%s)", x); break;
            default: sprintf(t, "%s", x);
        }
        if (dfmt == 1) {   // F16 result in the low half, high half zero
            char h[2200];
            snprintf(h, sizeof h, "(uint(as_type<ushort>(half(%s))))", bits(w, 50, 1) ? (sprintf(u, "saturate(%s)", t), u) : t);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3810000000000000ull, 0xfef8000000000000ull)) {   // IADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) c->uses_cc = true;
        sprintf(t, "(%s%s + %s%s%s)", bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
        if (bits(w, 47, 1)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "", R(a),
               bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      r%d = res;\n", d);
            }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1c00000000000000ull, 0xfe00000000000000ull)) {   // IADD32I (CC bit 52, X bit 53)
        bool wcc = bits(w, 52, 1), x = bits(w, 53, 1);
        if (wcc || x) c->uses_cc = true;
        sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(0x%08xu)%s;%s\n", bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32),
           x ? " + ulong(cc)" : "", wcc ? " cc = uint(sum >> 32) & 1u;" : "");
        if (d != RZ) {
            c->reg_used[d] = 1;
            sp(&c->body, "      r%d = uint(sum);\n", d);
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x38c0000000000000ull, 0xfef0000000000000ull)) {   // IADD3
        bool wcc3 = bits(w, 47, 1), x3 = bits(w, 48, 1);
        if (wcc3 || x3) {   // carry in/out (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(%s%s) + ulong(%s%s)%s;\n", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
               bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? " + ulong(cc)" : "");
            if (wcc3) sp(&c->body, "      cc = uint(sum >> 32) & 1u;\n");
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      r%d = uint(sum);\n", d);
            }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s%s + %s%s + %s%s)", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false), bits(w, 49, 1) ? "-" : "",
                    R((int)bits(w, 39, 8)));
            set_reg(c, d, t);
        }
    } else if (OPM(0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3818000000000000ull, 0xfef8000000000000ull)) {   // ISCADD
        sprintf(t, "((%s%s << %du) + %s%s)", bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
        if (bits(w, 47, 1)) {   // .CC: carry out for a following IADD.X (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { uint s = %s%s << %du; uint t2 = %s%s; uint res = s + t2; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "", R(a),
               (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      r%d = res;\n", d);
            }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1400000000000000ull, 0xfc00000000000000ull)) {   // ISCADD32I
        sprintf(t, "((%s << %du) + 0x%08xu)", R(a), (int)bits(w, 53, 5), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5a00000000000000ull, 0xff80000000000000ull) || OPM(0x5200000000000000ull, 0xff80000000000000ull) ||
               OPM(0x4a00000000000000ull, 0xff80000000000000ull) || OPM(0x3400000000000000ull, 0xfe80000000000000ull)) {   // IMAD
        /* d = [0,8), a = [8,16), B by the opcode class (reg 0x5a, cbuf 0x4a, imm 0x34) and c = REG_39; the 0x52 form has B = REG_39 and c = a cbuf.
         * Layout per envytools envydis gm107 (MIT): bit 48 A and B signed, 53 C signed, 54 .HI (high word of the 64-bit product, plus c), 50 .SAT, 49 .X,
         * 47 .CC. (An earlier version read bits 49/51 as negations and 53 as .HI, from memory; no real shader in the LAS or SMO corpora uses IMAD.)
         * The saturating and carry forms fail cleanly. */
        char cb[128], pr[256];
        const char *bv, *cv;
        if ((w >> 56) == 0x52) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = B(false);
            cv = R((int)bits(w, 39, 8));
        }
        bool sgn = bits(w, 48, 1), hi = bits(w, 54, 1);
        if (bits(w, 47, 1) || bits(w, 49, 1) || bits(w, 50, 1)) {
            fail(c, "imad carry/saturate forms (.CC/.X/.SAT)");
            return;
        }
        if (hi) {
            if (sgn) sprintf(pr, "ulong(long(int(%s)) * long(int(%s)))", R(a), bv);
            else sprintf(pr, "(ulong(%s) * ulong(%s))", R(a), bv);
            sprintf(t, "(uint(%s >> 32) + %s)", pr, cv);
        } else {
            sprintf(t, "(%s * %s + %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5bd0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bd0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36d0000000000000ull, 0xfef8000000000000ull)) {   // LEA (.LO, and .HI in the reg and cbuf forms)
        /* Bit layout from memory; no real word in the corpora. B by the opcode class, bit 45 negates B. .LO (opcode bit 51 clear): d = (a << scale) + B,
         * scale = [39,44), .CC bit 47 writes the carry out, .X bit 48 adds the carry in. .HI (bit 51 set, not in the imm class): the high word of the
         * 64-bit value {c:a} << scale, plus B, with scale = [28,33), c = REG_39 and .X at bit 48 (as in .LO). They build 64-bit address arithmetic with IADD.X. */
        bool hi = bits(w, 51, 1), negb = bits(w, 45, 1);
        char bn[160];
        sprintf(bn, negb ? "(0u - %s)" : "%s", B(false));
        if (hi) {
            int sc = (int)bits(w, 28, 5);
            bool x = bits(w, 48, 1);
            const char *cv = R((int)bits(w, 39, 8));
            if (x) c->uses_cc = true;
            if (sc) sprintf(t, "(((%s << %du) | (%s >> %du)) + %s%s)", cv, sc, R(a), 32 - sc, bn, x ? " + cc" : "");
            else sprintf(t, "(%s + %s%s)", cv, bn, x ? " + cc" : "");
            set_reg(c, d, t);
        } else {
            int sc = (int)bits(w, 39, 5);
            bool wcc = bits(w, 47, 1), x = bits(w, 48, 1);
            if (wcc || x) {
                c->uses_cc = true;
                sp(&c->body, "    { ulong sum = ulong(%s << %du) + ulong(%s)%s;\n", R(a), sc, bn, x ? " + ulong(cc)" : "");
                if (wcc) sp(&c->body, "      cc = uint(sum >> 32) & 1u;\n");
                if (d != RZ) {
                    c->reg_used[d] = 1;
                    c->reg_cb[d] = -1;
                    sp(&c->body, "      r%d = uint(sum);\n", d);
                }
                sp(&c->body, "    }\n");
            } else {
                sprintf(t, "((%s << %du) + %s)", R(a), sc, bn);
                set_reg(c, d, t);
            }
        }
    } else if (OPM(0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3660000000000000ull, 0xfef0000000000000ull)) {   // ISETP
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pp);
        if (p != PT) {
            c->pred_used[p] = 1;
            sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo));
        }
        if (q != PT) {
            c->pred_used[q] = 1;
            sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo));
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3650000000000000ull, 0xfef0000000000000ull)) {   // ISET
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3820000000000000ull, 0xfef8000000000000ull)) {   // IMNMX
        bool sgn = bits(w, 48, 1);
        const char *x = R(a), *y = B(false), *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (sgn) sprintf(t, "uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))", p, x, y, x, y);
        else sprintf(t, "(%s ? min(%s, %s) : max(%s, %s))", p, x, y, x, y);
        set_reg(c, d, t);
    } else if (OPM(0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3848000000000000ull, 0xfef8000000000000ull)) {   // SHL
        const char *bv = B(false);
        if (bits(w, 39, 1)) sprintf(t, "(%s << (%s & 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s << (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3828000000000000ull, 0xfef8000000000000ull)) {   // SHR
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (sgn) sprintf(t, "uint(int(%s) >> min(%s, 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s >> (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3840000000000000ull, 0xfef8000000000000ull)) {   // LOP
        char x[64], y[256];
        sprintf(x, bits(w, 39, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 41, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            int pop = (int)bits(w, 44, 2);
            sp(&c->body, "    { uint res = %s;\n", t);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      r%d = res;\n", d);
            }
            sp(&c->body, "      p%d = %s; }\n", pd, pop == 2 ? "(res == 0u)" : pop == 3 ? "(res != 0u)" : pop == 1 ? "true" : "false");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x0400000000000000ull, 0xfc00000000000000ull)) {   // LOP32I
        char x[64], y[64];
        sprintf(x, bits(w, 55, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 56, 1) ? "(~0x%08xu)" : "0x%08xu", (uint32_t)bits(w, 20, 32));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 53, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        set_reg(c, d, t);
    } else if (OPM(0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(0x0200000000000000ull, 0xfe00000000000000ull)) {   // LOP3
        uint32_t lut;
        const char *bv, *cv;
        char bb[256];
        if ((w >> 56) == 0x5b) {
            lut = (uint32_t)bits(w, 28, 8);
            bv = R((int)bits(w, 20, 8));
            cv = R((int)bits(w, 39, 8));
        } else if ((w >> 58) == 0xf) {
            lut = (uint32_t)bits(w, 48, 8);
            sprintf(bb, "0x%08xu", (uint32_t)((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0)));
            bv = bb;
            cv = R((int)bits(w, 39, 8));
        } else {
            lut = (uint32_t)bits(w, 48, 8);
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb);
            cv = R((int)bits(w, 39, 8));
        }
        char e[1024] = "0u";
        // sum of minterms
        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            char term[256];
            sprintf(term, "(%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", R(a), (i & 2) ? "" : "~", bv, (i & 1) ? "" : "~", cv);
            if (first) {
                strcpy(e, term);
                first = 0;
            } else {
                strcat(e, " | ");
                strcat(e, term);
            }
        }
        sprintf(t, "(%s)", e);
        set_reg(c, d, t);
    } else if (OPM(0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3800000000000000ull, 0xfef8000000000000ull)) {   // BFE
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        /* .BREV: the source is bit-reversed before the field is extracted */
        if (bits(w, 40, 1)) sprintf(t, sgn ? "bfe_s(reverse_bits(%s), %s)" : "bfe_u(reverse_bits(%s), %s)", R(a), bv);
        else sprintf(t, sgn ? "bfe_s(%s, %s)" : "bfe_u(%s, %s)", R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {   // BFI
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            sprintf(t, "bfi(%s, %s, %s)", R(a), cv, bv);   // insert a into base (cbuf) with b = position/length
            // bfi with cbuf form: B = REG_39 as position/length, C = cbuf as base
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        } else {
            bv = B(false);
            cv = R((int)bits(w, 39, 8));
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3600000000000000ull, 0xfec0000000000000ull) || OPM(0x5100000000000000ull, 0xff80000000000000ull)) {   // XMAD
        // per Ryujinx's InstXmad* decoders and EmitXmad
        const char *bv, *cv;
        char cb[256], bb[256];
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {   // reg, reg
            bv = R((int)bits(w, 20, 8));
            cv = R((int)bits(w, 39, 8));
            hb = bits(w, 35, 1);
            psl = bits(w, 36, 1);
            mrg = bits(w, 37, 1);
            ext = bits(w, 38, 1);
            cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {   // cbuf B
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            cv = R((int)bits(w, 39, 8));
            hb = bits(w, 52, 1);
            psl = bits(w, 55, 1);
            mrg = bits(w, 56, 1);
            ext = bits(w, 54, 1);
            cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {   // imm16 B
            sprintf(bb, "0x%xu", (uint32_t)bits(w, 20, 16));
            bv = bb;
            cv = R((int)bits(w, 39, 8));
            hb = false;
            psl = bits(w, 36, 1);
            mrg = bits(w, 37, 1);
            ext = bits(w, 38, 1);
            cmode = (int)bits(w, 50, 3);
        } else {   // 0x51: B = reg39, C = cbuf
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            hb = bits(w, 52, 1);
            psl = false;
            mrg = false;
            ext = bits(w, 54, 1);
            cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        if (ext || wcc) c->uses_cc = true;
        sp(&c->body, "    { uint xa = (%s >> %d) & 0xFFFFu; uint xb = (%s >> %d) & 0xFFFFu; uint xbu = %s;\n", R(a), ha ? 16 : 0, bv, hb ? 16 : 0, bv);
        if (sa) sp(&c->body, "      xa = uint(int(short(xa)));\n");
        if (sb) sp(&c->body, "      xb = uint(int(short(xb)));\n");
        sp(&c->body, "      uint prod = xa * xb;%s\n", psl ? " prod <<= 16;" : "");
        switch (cmode) {
            case 1: sp(&c->body, "      uint cc_ = %s & 0xFFFFu;\n", cv); break;                                     // clo
            case 2: sp(&c->body, "      uint cc_ = %s >> 16;\n", cv); break;                                         // chi
            case 3: sp(&c->body, "      uint cc_ = %s - (((xa >> 31) << 16) + ((xb >> 31) << 16));\n", cv); break;   // csfu
            case 4: sp(&c->body, "      uint cc_ = %s + (xbu << 16);\n", cv); break;                                 // cbcc
            default: sp(&c->body, "      uint cc_ = %s;\n", cv);                                                     // cfull
        }
        if (ext) sp(&c->body, "      uint res = prod + (cc & 1u);\n");
        else sp(&c->body, "      uint res = prod + cc_;\n");
        if (wcc) sp(&c->body, "      cc = (res < prod) ? 1u : 0u;\n");
        if (mrg) sp(&c->body, "      res = (res & 0xFFFFu) | (xbu << 16);\n");
        if (d != RZ) {
            c->reg_used[d] = 1;
            sp(&c->body, "      r%d = res;\n", d);
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5090000000000000ull, 0xfff8000000000000ull)) {   // PSETP
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        const char *pcc = P((int)bits(w, 39, 3), bits(w, 42, 1));
        int bo1 = (int)bits(w, 24, 2), bo2 = (int)bits(w, 45, 2);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3);
        sp(&c->body, "    { bool v = (%s %s %s); bool pp = %s;\n", pa, bool_op(bo1), pb, pcc);
        if (p != PT) {
            c->pred_used[p] = 1;
            sp(&c->body, "      p%d = v %s pp;\n", p, bool_op(bo2));
        }
        if (q != PT) {
            c->pred_used[q] = 1;
            sp(&c->body, "      p%d = !v %s pp;\n", q, bool_op(bo2));
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5088000000000000ull, 0xfff8000000000000ull)) {   // PSET
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        sprintf(t, "(((%s %s %s)) %s %s)", pa, bool_op((int)bits(w, 24, 2)), pb, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0xf0c8000000000000ull, 0xfff8000000000000ull)) {   // S2R
        int sr = (int)bits(w, 20, 8);
        switch (sr) {
            /* SR_Y_DIRECTION: -1 under the lower-left window origin (NVN's default; LASRecomp answers -1 for LAS, nxnative LAS notes), +1 under upper-left
         * (Ryujinx's constant, right for the upper-left titles it runs). The origin is the guest's, from the pipeline key. */
            case 0x12: set_reg(c, d, c->key && c->key->upper_left ? "0x3f800000u" : "0xbf800000u"); break;
            case 0x11: set_reg(c, d, c->stage == STAGE_TESS_CTRL || c->stage == STAGE_GEOMETRY ? "inv" : "0u"); break;   // SR_INVOCATION_ID: the output control point of a tess control thread; the geometry instance (gid % sp[3])
            case 0x21: set_reg(c, d, c->stage == STAGE_COMPUTE || c->stage == STAGE_TESS_CTRL ? "tid.x" : "0u"); break;
            case 0x22: set_reg(c, d, "tid.y"); break;
            case 0x23: set_reg(c, d, "tid.z"); break;
            case 0x25: set_reg(c, d, "ctaid.x"); break;
            case 0x26: set_reg(c, d, "ctaid.y"); break;
            case 0x27: set_reg(c, d, "ctaid.z"); break;
            case 0x00: set_reg(c, d, "0u"); break;
            default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);
        }
    } else if (OPM(0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3830000000000000ull, 0xfef8000000000000ull)) {   // FLO
        const char *bv = B(false);
        sprintf(t, bits(w, 48, 1) ? "(int(%s) < 0 ? (31u - clz(~%s)) : (31u - clz(%s)))" : "(31u - clz(%s))", bv, bv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3808000000000000ull, 0xfef8000000000000ull)) {   // POPC
        sprintf(t, bits(w, 40, 1) ? "popcount(~%s)" : "popcount(%s)", B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3838000000000000ull, 0xfef8000000000000ull)) {   // IMUL
        if (bits(w, 39, 1)) sprintf(t, bits(w, 40, 1) ? "uint(mulhi(int(%s), int(%s)))" : "mulhi(%s, %s)", R(a), B(false));
        else sprintf(t, "(%s * %s)", R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x1f00000000000000ull, 0xff00000000000000ull)) {   // IMUL32I
        sprintf(t, "(%s * 0x%08xu)", R(a), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38e0000000000000ull, 0xfef8000000000000ull)) {   // I2I
        int dt = (int)((bits(w, 8, 2)) | (bits(w, 12, 1) << 2)), st = (int)((bits(w, 10, 2)) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        int ssz = st & 3;
        bool ssg = st & 4;
        if (ssz == 0) sprintf(src, ssg ? "int(char((%s >> %d) & 0xFFu))" : "int((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (ssz == 1) sprintf(src, ssg ? "int(short((%s >> %d) & 0xFFFFu))" : "int((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, "int(%s)", bv);
        if (bits(w, 49, 1)) {
            sprintf(u, "abs(%s)", src);
            strcpy(src, u);
        }
        if (bits(w, 45, 1)) {
            sprintf(u, "(-%s)", src);
            strcpy(src, u);
        }
        int dsz = dt & 3;
        bool dsg = dt & 4, sat = bits(w, 50, 1);
        if (dsz == 0) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -128, 127))" : "uint(clamp(%s, 0, 255))") : "(uint(%s) & 0xFFu)", src);
        else if (dsz == 1) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -32768, 32767))" : "uint(clamp(%s, 0, 65535))") : "(uint(%s) & 0xFFFFu)", src);
        else sprintf(t, "uint(%s)", src);
        set_reg(c, d, t);
        if (bits(w, 47, 1)) {   // .CC: zero/sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", t);
        }
    } else if (OPM(0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {   // PRMT
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = B(false);
            cv = R((int)bits(w, 39, 8));
        }
        int pmode = (int)bits(w, 48, 4);   // 0 default, 1 F4E, 2 B4E, 3 RC8, 4 ECL, 5 ECR, 6 RC16
        if (pmode > 6) {
            fail(c, "prmt mode %d", pmode);
            return;
        }
        if (pmode) sprintf(t, "prmt_m(%s, %s, %s, %uu)", R(a), cv, bv, pmode);
        else sprintf(t, "prmt(%s, %s, %s)", R(a), cv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(0x36f8000000000000ull, 0xfef8000000000000ull)) {   // SHF
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        const char *sh = ((w >> 60) == 3) ? B(false) : R((int)bits(w, 20, 8));
        const char *hi = R((int)bits(w, 39, 8));
        if (right) sprintf(t, "uint((((ulong(%s) << 32) | ulong(%s)) >> (%s & 31u)))", hi, R(a), sh);
        else sprintf(t, "uint((((ulong(%s) << 32) | ulong(%s)) << (%s & 31u)) >> 32)", hi, R(a), sh);
        set_reg(c, d, t);
    } else if (OPM(0xeed0000000000000ull, 0xfff8000000000000ull) || OPM(0xeed8000000000000ull, 0xfff8000000000000ull)) {   // LDG / STG
        bool store = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int size = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        c->info->uses_gmem = true;
        if (store) c->info->uses_gmem_store = true;
        char addr[256];
        if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
        else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
        else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
        if (maxwell_gmem_size == ~0ull) {
            // GPU-address mode: the address is a Metal GPU virtual address (driver constants hold it); `gmem` becomes that pointer and `ga` the byte offset
            sp(&c->body, "    { ulong ga0 = %s; bool ok = ga0 != 0ul; device uchar *gmem = (device uchar *)ga0; const ulong ga = 0ul;\n", addr);
        } else
            sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul;\n", addr, (unsigned long long)maxwell_gmem_base,
               (unsigned long long)maxwell_gmem_size);
        static const char *const ld_small[] = {"uint(gmem[ga])", "uint(int(char(gmem[ga])))", "uint(*(device ushort *)(gmem + ga))",
                                               "uint(int(*(device short *)(gmem + ga)))"};
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        if (!store) {
            if (size < 4) {
                if (d != RZ) {
                    c->reg_used[d] = 1;
                    sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]);
                }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) {
                        c->reg_used[d + k] = 1;
                        sp(&c->body, "      r%d = ok ? *(device uint *)(gmem + ga + %du) : 0u;\n", d + k, k * 4);
                    }
            }
        } else {
            if (size == 0 || size == 1) sp(&c->body, "      if (ok) gmem[ga] = uchar(%s);\n", R(d));
            else if (size == 2 || size == 3) sp(&c->body, "      if (ok) *(device ushort *)(gmem + ga) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++) sp(&c->body, "      if (ok) *(device uint *)(gmem + ga + %du) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
    } else if (emit_half(c, w)) {
    } else if (OPM(0x50f8000000000000ull, 0xfff8000000000000ull)) {   // FSWZADD
        // per quad lane (laneid & 3), 2 bits of the swizzle pick: a+b, b-a, a-b, b
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        if (c->stage != STAGE_FRAGMENT) {
            sprintf(t, "as_type<uint>(%s + %s)", F(R(a)), F(R((int)bits(w, 20, 8))));
        } else {
            c->uses_lane = true;
            sprintf(t, "as_type<uint>(fswzadd(%s, %s, 0x%02xu, lane))", F(R(a)), F(R((int)bits(w, 20, 8))), swz);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3640000000000000ull, 0xfef0000000000000ull) || OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {   // ICMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, false, bb);
            cv = R((int)bits(w, 39, 8));
        }
        icmp_expr(cmp, (int)bits(w, 49, 3), cv, "0u", bits(w, 48, 1));
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {   // LDL / STL
        // thread-private local memory, modelled as a word array (byte/short accesses within a word)
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        c->uses_local = true;
        char addr[128];
        if (a == RZ) sprintf(addr, "%du", off);
        else sprintf(addr, "(%s + %du)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        sp(&c->body, "    { uint la = %s;\n", addr);
        if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                if (d != RZ) {
                    c->reg_used[d] = 1;
                    sp(&c->body, "      uint v = lmem[(la >> 2) & %du]; uint sh = (la & 3u) * 8u; r%d = %s;\n", LMEM_WORDS - 1, d, ext[size]);
                }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) {
                        c->reg_used[d + k] = 1;
                        sp(&c->body, "      r%d = lmem[((la >> 2) + %du) & %du];\n", d + k, k, LMEM_WORDS - 1);
                    }
            }
        } else {
            if (size < 4) {
                uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                sp(&c->body, "      uint sh = (la & 3u) * 8u; uint i = (la >> 2) & %du; lmem[i] = (lmem[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   LMEM_WORDS - 1, m, R(d), m);
            } else {
                for (int k = 0; k < nw; k++) sp(&c->body, "      lmem[((la >> 2) + %du) & %du] = %s;\n", k, LMEM_WORDS - 1, R(d == RZ ? RZ : d + k));
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef10000000000000ull, 0xfff8000000000000ull)) {   // SHFL
        int mode = (int)bits(w, 30, 2);
        char bb[32];
        const char *bv = bits(w, 28, 1) ? (sprintf(bb, "%uu", (unsigned)bits(w, 20, 5)), bb) : R((int)bits(w, 20, 8));
        int pd = (int)bits(w, 48, 3);
        if (c->stage == STAGE_VERTEX) {
            set_reg(c, d, R(a));   // no SIMD-group functions in vertex functions: lane-local value
        } else if (mode == 3) {
            bool quad = bits(w, 28, 1) && bits(w, 20, 5) < 4;
            sprintf(t, quad ? "quad_shuffle_xor(%s, ushort(%s))" : "simd_shuffle_xor(%s, ushort(%s))", R(a), bv);
            set_reg(c, d, t);
        } else {
            static const char *const fn[] = {"simd_shuffle", "simd_shuffle_up", "simd_shuffle_down"};
            sprintf(t, "%s(%s, ushort(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        }
        if (pd != PT) {
            c->pred_used[pd] = 1;
            sp(&c->body, "    p%d = true;\n", pd);
        }
    } else if (OPM(0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4cf0000000000000ull, 0xfff8000000000000ull)) {   // R2P
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {   // .CC: the flags Z, S, C (bits 0-2; overflow isn't tracked) take bits of Ra where the mask selects them
            c->uses_cc = true;
            sp(&c->body, "    { uint rv = %s >> %du, rm = %s;\n", R(a), shift, mask);
            sp(&c->body,
               "      if (rm & 1u) ccz = (rv & 1u) != 0u;\n      if (rm & 2u) ccs = (rv & 2u) != 0u;\n      if (rm & 4u) cc = (rv >> 2) & 1u;\n    }\n");
            goto r2p_done;
        }
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, "    if ((%s >> %du) & 1u) p%d = ((%s >> %du) & 1u) != 0u;\n", mask, i, i, R(a), shift + i);
        }
    r2p_done:;
    } else if (OPM(0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4ce8000000000000ull, 0xfff8000000000000ull)) {   // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {   // .CC: the flags Z, S, C, O as bits 0-3 (overflow isn't tracked: 0)
            c->uses_cc = true;
            sp(&c->body, "    { uint pr = (ccz ? 1u : 0u) | (ccs ? 2u : 0u) | ((cc & 1u) << 2);\n");
            sp(&c->body, "      uint m = (%s & 0xFu) << %du;\n", mask, shift);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift);
            }
            sp(&c->body, "    }\n");
            goto p2r_done;
        }
        sp(&c->body, "    { uint pr = 0u;\n");
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, "      pr |= (p%d ? 1u : 0u) << %du;\n", i, i);
        }
        sp(&c->body, "      uint m = (%s & 0x7Fu) << %du;\n", mask, shift);
        if (d != RZ) {
            c->reg_used[d] = 1;
            sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift);
        }
        sp(&c->body, "    }\n");
    p2r_done:;
    } else if (OPM(0xe250000000000000ull, 0xfff0000000000020ull)) {   // BRX reg + imm (relative)
        // computed targets can name a scheduling word; execution continues at the next slot (as for BRA)
        sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; pc = t; continue; }\n", pc + 8, R(a), (uint32_t)sbits(w, 20, 24));
    } else if (OPM(0x50a0000000000000ull, 0xfff8000000000000ull)) {   // CSETP (zero/sign flags only)
        int test = (int)bits(w, 8, 5);
        const char *cond;
        switch (test) {
            case 0: cond = "false"; break;
            case 15: cond = "true"; break;
            case 2:
            case 10: cond = "ccz"; break;
            case 5:
            case 13: cond = "!ccz"; break;
            case 1:
            case 9: cond = "ccs"; break;
            case 6:
            case 14: cond = "!ccs"; break;
            case 4:
            case 12: cond = "(!ccz && !ccs)"; break;
            case 3:
            case 11: cond = "(ccz || ccs)"; break;
            default: cond = "false"; fail(c, "csetp test %d", test);
        }
        c->uses_cc = true;
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cond, pc_);
        if (p != PT) {
            c->pred_used[p] = 1;
            sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo));
        }
        if (q != PT) {
            c->pred_used[q] = 1;
            sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo));
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5098000000000000ull, 0xfff8000000000000ull)) {   // CSET
        fail(c, "cset");
    } else if (OPM(0xef48000000000000ull, 0xfff8000000000000ull) || OPM(0xef58000000000000ull, 0xfff8000000000000ull)) {   // LDS / STS
        // threadgroup (shared) memory: a byte array over `threadgroup uint smem_w[]`. Out-of-range accesses read 0 / are dropped (the hardware faults).
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        if (c->stage != STAGE_COMPUTE) {
            fail(c, "shared memory (LDS/STS) outside a compute shader");
            return;
        }
        c->uses_smem = true;
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        int nbytes = size < 2 ? 1 : size < 4 ? 2 : nw * 4;
        char addr[128];
        if (a == RZ) {
            sprintf(addr, "%uu", (uint32_t)off);
            if (off >= 0 && (uint32_t)off + nbytes > c->smem_need) c->smem_need = (uint32_t)off + nbytes;
        } else {
            sprintf(addr, "(%s + %uu)", R(a), (uint32_t)off);
            c->smem_dyn = true;
        }
        sp(&c->body, "    { uint sa = %s; bool ok = sa + %uu <= SMEM_BYTES;\n", addr, (unsigned)nbytes);
        if (!store) {
            static const char *const ld_small[] = {"uint(smem[sa])", "uint(int(char(smem[sa])))", "uint(*(threadgroup ushort *)(smem + sa))",
                                                   "uint(int(*(threadgroup short *)(smem + sa)))"};
            if (size < 4) {
                if (d != RZ) {
                    c->reg_used[d] = 1;
                    sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]);
                    c->reg_cb[d] = -1;
                }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) {
                        c->reg_used[d + k] = 1;
                        c->reg_cb[d + k] = -1;
                        sp(&c->body, "      r%d = ok ? *(threadgroup uint *)(smem + sa + %uu) : 0u;\n", d + k, k * 4);
                    }
            }
        } else {
            if (size < 2) sp(&c->body, "      if (ok) smem[sa] = uchar(%s);\n", R(d));
            else if (size < 4) sp(&c->body, "      if (ok) *(threadgroup ushort *)(smem + sa) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++) sp(&c->body, "      if (ok) *(threadgroup uint *)(smem + sa + %uu) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xec00000000000000ull, 0xff00000000000000ull)) {   // ATOMS (shared memory atomic)
        // layout from the Maxwell tables, unverified against real shaders: Rd old value, Ra address, Rb operand, offset 30-51, size 28-29, op 52-55
        int op = (int)bits(w, 52, 4), size = (int)bits(w, 28, 2), rb = (int)bits(w, 20, 8);
        int32_t off = (int32_t)sbits(w, 30, 22);
        if (c->stage != STAGE_COMPUTE) {
            fail(c, "shared memory atomic outside a compute shader");
            return;
        }
        if (op > 8) {
            fail(c, "atoms op %d (CAS and float forms are not translated)", op);
            return;
        }
        if (size > 1) {
            fail(c, "atoms 64-bit");
            return;
        }
        c->uses_smem = true;
        c->atom_tg = true;
        char addr[128];
        if (a == RZ) {
            sprintf(addr, "%uu", (uint32_t)off);
            if (off >= 0 && (uint32_t)off + 4 > c->smem_need) c->smem_need = (uint32_t)off + 4;
        } else {
            sprintf(addr, "(%s + %uu)", R(a), (uint32_t)off);
            c->smem_dyn = true;
        }
        sp(&c->body, "    { uint sa = %s; bool ok = sa + 4u <= SMEM_BYTES;\n", addr);
        sp(&c->body, "      uint old = ok ? atom_tg((threadgroup atomic_uint *)(smem + sa), %uu, %s, %s) : 0u;\n", op, R(rb), size == 1 ? "true" : "false");
        if (d != RZ) {
            c->reg_used[d] = 1;
            c->reg_cb[d] = -1;
            sp(&c->body, "      r%d = old;\n", d);
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xed00000000000000ull, 0xff00000000000000ull) || OPM(0xebf8000000000000ull, 0xfff8000000000000ull)) {   // ATOM / RED (global memory atomic)
        // layout from the Maxwell tables, unverified against real shaders. ATOM: Rd old value, Ra address, Rb operand, offset 28-47, E 48, size 49-51, op 52-55.
        // RED: no result; operand in the Rd field, size 20-22, op 23-25.
        bool red = (w >> 56) == 0xeb;
        int op = red ? (int)bits(w, 23, 3) : (int)bits(w, 52, 4), size = red ? (int)bits(w, 20, 3) : (int)bits(w, 49, 3);
        int rb = red ? d : (int)bits(w, 20, 8);
        bool wide = bits(w, 48, 1);
        int64_t off = sbits(w, 28, 20);
        if (op > 8) {
            fail(c, "%s op %d (CAS and float forms are not translated)", red ? "red" : "atom", op);
            return;
        }
        if (size > 1) {
            fail(c, "%s of size %d (only 32-bit integer forms are translated)", red ? "red" : "atom", size);
            return;
        }
        c->info->uses_gmem = true;
        c->info->uses_gmem_store = true;
        char addr[256];
        if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
        else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
        else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
        c->atom_dev = true;
        if (maxwell_gmem_size == ~0ull)
            sp(&c->body, "    { ulong ga0 = %s; bool ok = ga0 != 0ul; device uchar *gmem = (device uchar *)ga0; const ulong ga = 0ul;\n", addr);
        else
            sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga + 4ul <= 0x%llxul;\n", addr, (unsigned long long)maxwell_gmem_base,
               (unsigned long long)maxwell_gmem_size);
        sp(&c->body, "      uint old = ok ? atom_dev((device atomic_uint *)(gmem + ga), %uu, %s, %s) : 0u;\n", op, R(rb), size == 1 ? "true" : "false");
        if (!red && d != RZ) {
            c->reg_used[d] = 1;
            c->reg_cb[d] = -1;
            sp(&c->body, "      r%d = old;\n", d);
        } else sp(&c->body, "      (void)old;\n");
        sp(&c->body, "    }\n");
    } else if (OPM(0x50d8000000000000ull, 0xfff8000000000000ull)) {   // VOTE
        // Rd = ballot of the active lanes' predicate, Pd (bits 45-47) = ALL / ANY / EQ (bits 48-49) of it. Vertex functions have no SIMD-group
        // operations: one lane votes alone.
        int vop = (int)bits(w, 48, 2), pd = (int)bits(w, 45, 3);
        if (vop > 2) {
            fail(c, "vote mode %d", vop);
            return;
        }
        sp(&c->body, "    { bool vp = %s;\n", P((int)bits(w, 39, 3), bits(w, 42, 1)));
        if (c->stage == STAGE_VERTEX) {
            if (d != RZ) {
                c->reg_used[d] = 1;
                c->reg_cb[d] = -1;
                sp(&c->body, "      r%d = vp ? 1u : 0u;\n", d);
            }
            if (pd != PT) {
                c->pred_used[pd] = 1;
                sp(&c->body, "      p%d = %s;\n", pd, vop == 2 ? "true" : "vp");
            }
        } else {
            if (d != RZ) {
                c->reg_used[d] = 1;
                c->reg_cb[d] = -1;
                sp(&c->body, "      r%d = uint(static_cast<ulong>(simd_ballot(vp)));\n", d);
            }
            if (pd != PT) {
                c->pred_used[pd] = 1;
                sp(&c->body,
                   vop == 0   ? "      p%d = simd_all(vp);\n"
                   : vop == 1 ? "      p%d = simd_any(vp);\n"
                              : "      p%d = simd_all(vp) || !simd_any(vp);\n",
                   pd);
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x50c8000000000000ull, 0xfff8000000000000ull)) {   // CS2R: 64-bit special register read (SRZ / clock counters)
        // no clock source is exposed to the shader: the pair reads 0 (SRZ is 0 by definition)
        for (int k = 0; k < 2; k++)
            if (d != RZ && d + k < RZ) {
                c->reg_used[d + k] = 1;
                c->reg_cb[d + k] = -1;
                sp(&c->body, "    r%d = 0u;\n", d + k);
            }
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {   // BAR
        // tess control invocations exchange their outputs through device memory, so the barrier orders those writes too
        sp(&c->body, "    threadgroup_barrier(%s);\n", c->stage == STAGE_TESS_CTRL ? "mem_flags::mem_device | mem_flags::mem_threadgroup" : "mem_flags::mem_threadgroup");
    } else if (OPM(0x5c70000000000000ull, 0xfff8000000000000ull) || OPM(0x4c70000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3870000000000000ull, 0xfef8000000000000ull)) {   // DADD (rounding 39-40, neg B 45, abs A 46, neg A 48, abs B 49)
        sprintf(t, "sd_add(%s, %s, %uu)", DNA(DR(a), bits(w, 48, 1), bits(w, 46, 1)), DNA(DB(), bits(w, 45, 1), bits(w, 49, 1)), (unsigned)bits(w, 39, 2));
        dstore(c, d, t);
    } else if (OPM(0x5c80000000000000ull, 0xfff8000000000000ull) || OPM(0x4c80000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3880000000000000ull,
                   0xfef8000000000000ull)) {   // DMUL (rounding 39-40, negate 48: applied to A so the rounding mode sees the negated product)
        sprintf(t, "sd_mul(%s, %s, %uu)", DNA(DR(a), bits(w, 48, 1), false), DB(), (unsigned)bits(w, 39, 2));
        dstore(c, d, t);
    } else if (OPM(0x5b70000000000000ull, 0xfff8000000000000ull) || OPM(0x4b70000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3670000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5370000000000000ull, 0xfff8000000000000ull)) {   // DFMA (neg B 48, neg C 49, rounding 50-51)
        const char *bv;
        char cbuf_c[320], bbuf[320];
        const char *cv = dop_c_dfma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "sd_fma(%s, %s, %s, %uu)", DR(a), DNA(bv, bits(w, 48, 1), false), DNA(cv, bits(w, 49, 1), false), (unsigned)bits(w, 50, 2));
        dstore(c, d, t);
    } else if (OPM(0x5b80000000000000ull, 0xfff8000000000000ull) || OPM(0x4b80000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3680000000000000ull,
                   0xfef8000000000000ull)) {   // DSETP (the FSETP layout: neg A 43, abs A 7, neg B 6, abs B 44, compare 48-51, combine 45-46)
        const char *x = DNA(DR(a), bits(w, 43, 1), bits(w, 7, 1)), *y = DNA(DB(), bits(w, 6, 1), bits(w, 44, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = sd_cmp(%s, %s, %uu) != 0u; bool pp = %s;\n", x, y, (unsigned)bits(w, 48, 4), pc_);
        if (p != PT) {
            c->pred_used[p] = 1;
            sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo));
        }
        if (q != PT) {
            c->pred_used[q] = 1;
            sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo));
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5900000000000000ull, 0xff80000000000000ull) || OPM(0x4900000000000000ull, 0xff80000000000000ull) ||
               OPM(0x3200000000000000ull,
                   0xfe80000000000000ull)) {   // DSET (the FSET layout; the result is one 32-bit register: 0xFFFFFFFF, or 1.0f with the BF bit 52)
        const char *x = DNA(DR(a), bits(w, 43, 1), bits(w, 54, 1)), *y = DNA(DB(), bits(w, 53, 1), bits(w, 44, 1));
        sprintf(t, "((sd_cmp(%s, %s, %uu) != 0u) %s %s)", x, y, (unsigned)bits(w, 48, 4), bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5c50000000000000ull, 0xfff8000000000000ull) || OPM(0x4c50000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3850000000000000ull, 0xfef8000000000000ull)) {   // DMNMX (the FMNMX layout: predicate true selects the minimum)
        const char *x = DNA(DR(a), bits(w, 48, 1), bits(w, 46, 1)), *y = DNA(DB(), bits(w, 45, 1), bits(w, 49, 1));
        sprintf(t, "sd_minmax(%s, %s, %s ? 0u : 1u)", x, y, P((int)bits(w, 39, 3), bits(w, 42, 1)));
        dstore(c, d, t);
    } else if (OPM(0x2000000000000000ull, 0xfc00000000000000ull)) {   // VADD
        video_op(c, w, d, "va + vb", true);
    } else if (OPM(0x5400000000000000ull, 0xff00000000000000ull)) {   // VABSDIFF
        video_op(c, w, d, "(va > vb ? va - vb : vb - va)", true);
    } else if (OPM(0x5f00000000000000ull, 0xff00000000000000ull)) {   // VMAD: a * b (>> 7 or 15 with the SHR bits 51-52) + Rc, no secondary op
        int shr = bits(w, 51, 2) == 1 ? 7 : bits(w, 51, 2) == 2 ? 15 : 0;
        bool sg = bits(w, 48, 1) || bits(w, 49, 1);   // Rc is signed when an operand is
        char e[200];
        sprintf(e, "((va * vb) >> %d) + long(%s(%s))", shr, sg ? "int" : "uint", R((int)bits(w, 39, 8)));
        video_op(c, w, d, e, false);
    } else if (OPM(0x5700000000000000ull, 0xff00000000000000ull) || OPM(0x5600000000000000ull, 0xff00000000000000ull)) {   // VSHL / VSHR
        /* The count is the 16-bit immediate at 20 (bit 50 narrows it to 8 bits); .W (bit 49) wraps it modulo 32, otherwise it clamps at 32. The B scheme of the
         * other video ops does not apply (bit 49 is .W here, not a sign), so B is forced to the unsigned immediate form. */
        bool left = (w >> 56) == 0x57;
        char e[200];
        if (bits(w, 49, 1)) sprintf(e, "(va %s (vb & 31l))", left ? "<<" : ">>");
        else sprintf(e, "(va %s min(vb & %s, 32l))", left ? "<<" : ">>", bits(w, 50, 1) ? "255l" : "65535l");
        video_op(c, w & ~(3ull << 49), d, e, true);
    } else if (OPM(0x4000000000000000ull, 0xfe00000000000000ull)) {   // VSET: Rd = (a cmp b) ? 1 : 0, then the secondary op with Rc
        char cb[32], e[96];
        sprintf(e, "(%s) ? 1l : 0l", video_cmp((int)(bits(w, 43, 2) | (bits(w, 47, 1) << 2)), cb));
        video_op(c, w, d, e, true);
    } else if (OPM(0x50f0000000000000ull, 0xfff8000000000000ull)) {   // VSETP: Pd = (a cmp b) bop Pc, Pq = !(a cmp b) bop Pc
        char ea[160], eb[160], cb[32];
        video_ab(c, w, ea, eb);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        sp(&c->body, "    { long va = %s, vb = %s; bool cmp = %s; bool pp = %s;\n", ea, eb, video_cmp((int)(bits(w, 43, 2) | (bits(w, 47, 1) << 2)), cb),
           P((int)bits(w, 39, 3), bits(w, 42, 1)));
        if (p != PT) {
            c->pred_used[p] = 1;
            sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo));
        }
        if (q != PT) {
            c->pred_used[q] = 1;
            sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo));
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x1000000000000000ull, 0xfc00000000000000ull)) {   // IMAD32I: Rd = (+-Ra * imm32 (.HI: its high word)) + (+-Rd); signed per bits 54 and 57
        bool sg = bits(w, 54, 1) || bits(w, 57, 1), hi = bits(w, 53, 1);
        if (bits(w, 52, 1)) {
            fail(c, "IMAD32I.CC is not supported");
            return;
        }
        char pr[300], acc[48];
        if (sg) sprintf(pr, "(long(int(%s)) * long(int(0x%08xu)))", R(a), (uint32_t)bits(w, 20, 32));
        else sprintf(pr, "(long(%s) * long(0x%08xu))", R(a), (uint32_t)bits(w, 20, 32));
        sprintf(acc, bits(w, 55, 1) ? "(0u - %s)" : "%s", R(d));
        sprintf(t, hi ? "(uint((%s%s%s) >> 32) + %s)" : "(uint(%s%s%s) + %s)", bits(w, 56, 1) ? "-" : "", pr, "", acc);
        set_reg(c, d, t);
    } else if (OPM(0x5c88000000000000ull, 0xfff8000000000000ull) || OPM(0x4c88000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3888000000000000ull, 0xfef8000000000000ull)) {   // FCHK (the .DIVIDE form): Pd = this division needs the slow path
        /* Conservative: true unless both operands are normal and the quotient's exponent keeps clear of the range ends, so the shader's fast reciprocal path
         * only runs when it is exact enough; whatever it rejects is computed by the slow path the compiler emitted. Pd at bits 3-5 (envydis). */
        int pd = (int)bits(w, 3, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
            sp(&c->body, "    { uint ea = (as_type<uint>(%s) >> 23) & 0xFFu, eb = (as_type<uint>(%s) >> 23) & 0xFFu; int eq = int(ea) - int(eb) + 127;\n", x,
               y);
            sp(&c->body, "      p%d = ea < 2u || ea > 253u || eb < 2u || eb > 253u || eq <= 24 || eq >= 253; }\n", pd);
        }
    } else if (OPM(0xefe8000000000000ull, 0xfff8000000000000ull)) {   // PIXLD: pixel/sample info; single-sample semantics (one sample, centred)
        /* mode at bits 31-33: 1 COVMASK, 2 COVERED, 3 OFFSET, 4 CENT_OFFSET, 5 MY_INDEX; Pd at bits 45-47. Coverage is the one sample, offsets and the index are 0. */
        int mode = (int)bits(w, 31, 3), pd = (int)bits(w, 45, 3);
        set_reg(c, d, mode == 1 || mode == 2 || mode == 0 ? "1u" : "0u");
        if (pd != PT) {
            c->pred_used[pd] = 1;
            sp(&c->body, "    p%d = %s;\n", pd, mode == 3 || mode == 4 || mode == 5 ? "false" : "true");
        }
    } else if (OPM(0x50d0000000000000ull, 0xfff8000000000000ull)) {   // LEPC: the address of this instruction (the unit the branch targets use)
        sprintf(t, "%uu", pc);
        set_reg(c, d, t);
    } else if (OPM(0xe260000000000000ull, 0xfff0000000000020ull)) {   // CAL: call, relative target; RET pops the return address
        c->uses_branches = true;
        sp(&c->body, "    cal[cal_n++] = %uu; pc = %uu; continue;\n", norm_target((int64_t)pc + 8), norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe220000000000000ull, 0xfff0000000000020ull)) {   // JCAL: call, absolute 32-bit target
        c->uses_branches = true;
        sp(&c->body, "    cal[cal_n++] = %uu; pc = %uu; continue;\n", norm_target((int64_t)pc + 8), norm_target((int64_t)bits(w, 20, 32)));
    } else if (OPM(0xe210000000000000ull, 0xfff0000000000020ull)) {   // JMP: absolute 32-bit target
        c->uses_branches = true;
        sp(&c->body, "    pc = %uu; continue;\n", norm_target((int64_t)bits(w, 20, 32)));
    } else if (OPM(0xe200000000000000ull, 0xfff0000000000020ull)) {   // JMX: absolute target Ra + imm32 (every instruction is a label, as for BRX)
        c->uses_branches = true;
        sp(&c->body, "    pc = %s + 0x%xu; continue;\n", R(a), (uint32_t)bits(w, 20, 32));
    } else if (OPM(0xe320000000000000ull, 0xfff0000000000000ull)) {   // RET: back to the caller (a RET with no caller ends the program)
        sp(&c->body, "    if (cal_n == 0u) { exit_now = true; break; }\n    pc = cal[--cal_n]; continue;\n");
    } else if (OPM(0xef60000000000000ull, 0xfff8000000000000ull) || OPM(0xef80000000000000ull, 0xfff8000000000000ull)) {
        // CCTL / CCTLL: cache control (invalidate, write back): no effect on the emulated memory
    } else if (OPM(0x5000000000000000ull, 0xff80000000000000ull)) {   // VABSDIFF4
        /* Four-lane byte absolute difference. Field layout (pinned by tests/t_xlate_stages.inc; the lane selectors follow VMNMX): A = [8,16), B = register [20,28)
         * when bit 50 is set else the 16-bit immediate at 20, C = [39,47), A signed 48, B signed 49, combine op [51,54): 0 = four packed byte differences
         * (C unused), 1 = ADD (sum of the four differences + C), 2 = MIN and 3 = MAX (per byte against C), SAT 54 (the ADD sum saturates at 2^32 - 1).
         * Lane selectors A [36,39) and B [28,31) must be 0 (the whole word). */
        int op = (int)bits(w, 51, 3);
        if (bits(w, 36, 3) || (bits(w, 50, 1) && bits(w, 28, 3))) {
            fail(c, "VABSDIFF4 with a lane selector (only the four-byte word form is translated)");
            return;
        }
        if (op > 3) {
            fail(c, "VABSDIFF4 combine op %d", op);
            return;
        }
        c->uses_vabsdiff4 = true;
        char eb[64];
        if (bits(w, 50, 1)) strcpy(eb, R((int)bits(w, 20, 8)));
        else sprintf(eb, "0x%xu", (uint32_t)bits(w, 20, 16));
        sprintf(t, "vabsdiff4(%s, %s, %s, %s, %s, %uu, %s)", R(a), eb, R((int)bits(w, 39, 8)), bits(w, 48, 1) ? "true" : "false", bits(w, 49, 1) ? "true" : "false",
                (unsigned)op, bits(w, 54, 1) ? "true" : "false");
        set_reg(c, d, t);
    } else if (OPM(0x53f8000000000000ull, 0xfff8000000000000ull) || OPM(0x53d8000000000000ull, 0xfff8000000000000ull)) {   // IDP
        /* Integer dot product with accumulate (the DP4A / DP2A family; Maxwell SM 5.x has no hardware form, so no compiler targeting it emits one and the field
         * layout below is this translator's own, pinned by tests/t_xlate.m): d = sum_i sel_i(Ra) * sel_i(Rb) + Rc, 32-bit wrap. Ra [8,16), Rb [20,28), Rc [39,47),
         * A signed bit 48, B signed bit 49. Class 0x53f8 = IDP.4A (the four bytes of Ra against the four bytes of Rb); class 0x53d8 = IDP.2A (the two halves of Ra
         * against two bytes of Rb: the low pair, or the high pair with bit 47 set). */
        bool two = OPM(0x53d8000000000000ull, 0xfff8000000000000ull);
        c->uses_idp = true;
        sprintf(t, "idp(%s, %s, %s, %s, %s, %s, %s)", R(a), R((int)bits(w, 20, 8)), R((int)bits(w, 39, 8)), bits(w, 48, 1) ? "true" : "false",
                bits(w, 49, 1) ? "true" : "false", two ? "true" : "false", bits(w, 47, 1) ? "true" : "false");
        set_reg(c, d, t);
    } else if (OPM(0x5a80000000000000ull, 0xff80000000000000ull) || OPM(0x5280000000000000ull, 0xff80000000000000ull) ||
               OPM(0x4a80000000000000ull, 0xff80000000000000ull) || OPM(0x3480000000000000ull, 0xfe80000000000000ull)) {   // IMADSP
        /* Multiply-add with sub-word operand selectors: d = sel_a(Ra) * sel_b(B) + C, low 32 bits. Layout (pinned by tests/t_xlate_stages.inc): B by the opcode
         * class (reg 0x5a, cbuf 0x4a, imm 0x34; the 0x52 form has B = REG_39 and C = a cbuf, as IMAD), C = REG_39, A selector [48,51), B selector [51,54), each
         * 0 U32, 1 S32, 2 U8, 3 S8, 4 U16, 5 S16 (the low byte / half of the operand, zero- or sign-extended). The .CC, .HI and .SAT forms (bits 47, 54, 55 of the
         * class) are refused. */
        int as = (int)bits(w, 48, 3), bs = (int)bits(w, 51, 3);
        if (as > 5 || bs > 5) {
            fail(c, "IMADSP operand selector %d", as > 5 ? as : bs);
            return;
        }
        if (bits(w, 47, 1) || bits(w, 54, 1)) {
            fail(c, "IMADSP carry/high forms (.CC/.HI)");
            return;
        }
        char cb[128];
        const char *bv, *cv;
        if ((w >> 56) == 0x52) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = B(false);
            cv = R((int)bits(w, 39, 8));
        }
        c->uses_isel = true;
        sprintf(t, "(isel(%s, %uu) * isel(%s, %uu) + %s)", R(a), (unsigned)as, bv, (unsigned)bs, cv);
        set_reg(c, d, t);
    } else if (OPM(0xe3a0000000000000ull, 0xfff0000000000000ull)) {   // BPT: debugger breakpoint / trap
        /* BPT.{INT,TRAP,PAUSE,DRAIN,CAL,...} raise a breakpoint for an attached debugger or the driver's trap handler. No debugger exists under the replay and the
         * emitted program has to keep running, so it is a documented no-op (a compiler emits it only for explicit device-side asserts in debug builds). */
    } else if (OPM(0xe390000000000000ull, 0xfff0000000000000ull)) {   // IDE: interrupt disable / enable
        // IDE.EN / IDE.DI mask the trap-handler interrupts of the warp; there is no interrupt to mask here: no-op
    } else if (OPM(0xe380000000000000ull, 0xfff0000000000000ull) || OPM(0xe370000000000000ull, 0xfff0000000000000ull)) {   // RAM / SAM
        // RAM / SAM save and restore the warp's address-mask registers around the trap handler: the handler never runs, so there is nothing to restore: no-op
    } else if (OPM(0xe360000000000000ull, 0xfff0000000000000ull)) {   // RTT: return from the trap handler
        // Control returns to the trapped program on hardware; the trap handler is never entered here, so a stray RTT can only end the thread, like EXIT
        sp(&c->body, c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe230000000000000ull, 0xfff0000000000020ull)) {   // PEXIT: set the target the next EXIT of this thread jumps to
        c->uses_branches = true;
        sp(&c->body, "    pex = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe280000000000000ull, 0xfff0000000000020ull)) {   // PLONGJMP: push a long-jump target
        c->uses_branches = true;
        sp(&c->body, "    ljs[ljs_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe310000000000000ull, 0xfff0000000000000ull)) {   // LONGJMP: pop the PLONGJMP stack and jump (as BRK does for PBK)
        c->uses_branches = true;
        sp(&c->body, "    pc = ljs[--ljs_n]; continue;\n");
    } else if (OPM(0xe2c0000000000000ull, 0xfff0000000000000ull)) {   // GETCRSPTR: Rd = call-return stack pointer (the CAL depth)
        set_reg(c, d, "cal_n");
    } else if (OPM(0xe2e0000000000000ull, 0xfff0000000000000ull)) {   // SETCRSPTR: call-return stack pointer = Ra (clamped to the stack)
        sp(&c->body, "    cal_n = min(%s, 16u);\n", R(a));
    } else if (OPM(0xe2d0000000000000ull, 0xfff0000000000000ull)) {   // GETLMEMBASE: Rd = local memory window base
        /* Local memory (LDL/STL) is a private array here, addressed from 0: the base is only a number the program reads back after SETLMEMBASE */
        c->uses_lmb = true;
        set_reg(c, d, "lmb");
    } else if (OPM(0xe2f0000000000000ull, 0xfff0000000000000ull)) {   // SETLMEMBASE: local memory window base = Ra
        c->uses_lmb = true;
        sp(&c->body, "    lmb = %s;\n", R(a));
    } else if (OPM(0xf0b8000000000000ull, 0xfff8000000000000ull)) {   // B2R: barrier state to register
        /* B2R.{RESULT,WARP} reads the convergence-barrier state (a Volta-era warp-sync save). The barriers here are Metal threadgroup barriers with no readable
         * state, so the register receives 0 (no thread blocked on a barrier). */
        set_reg(c, d, "0u");
    } else if (OPM(0xf0c0000000000000ull, 0xfff8000000000000ull)) {   // R2B: register to barrier state
        // The inverse of B2R: the barrier state is not materialised, so the write is dropped: no-op
    } else if (OPM(0xefd0000000000000ull, 0xfff8000000000000ull)) {   // ISBERD: read the attribute (ISBE) memory at the address in Ra
        /* Rd = a[Ra + imm10 at 20]: the indexed attribute read (AL2P + ALD.PHYS family) in the stage's own input space, so it shares the ALD register-addressed path */
        c->ald_vertex = RZ;
        c->ald_patch = false;
        c->ald_out = false;
        sp(&c->body, "    { uint ix = %s;\n", R(a));
        char ib[48];
        sprintf(ib, "(ix + %uu)", (uint32_t)bits(w, 20, 10));
        attr_load(c, d, 0, ib);
        sp(&c->body, "    }\n");
    } else if (OPM(0xeea0000000000000ull, 0xfff8000000000000ull)) {   // STP: store to the trap-handler parameter area
        // STP writes the state area the trap handler reads back (with RTT/RAM/SAM); with no handler the store has no observer: no-op
    } else if (OPM(0xfbe0000000000000ull, 0xfff8000000000000ull) || OPM(0xf6e0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xebe0000000000000ull, 0xfff8000000000000ull)) {   // OUT (reg, imm, cbuf forms)
        /* Geometry shader output: mode [39,41) 1 EMIT (append the current output vertex to the strip), 2 CUT (end the strip), 3 EMIT then CUT. The stream
         * operand is ignored (stream 0 only). Rd receives the number of vertices emitted so far. Emulated per input primitive in the geometry compute kernel. */
        if (c->stage != STAGE_GEOMETRY) {
            fail(c, "OUT: geometry shader output outside a geometry shader (stage %d)", c->stage);
            return;
        }
        int mode = (int)bits(w, 39, 2);
        c->uses_gs_emit = true;
        if (mode & 1)
            sp(&c->body,
               "    if (gs_n < GS_MAXV) { gs_put(gs_vtx + (gid * GS_MAXV + gs_n) * %uu, gso); gs_idx[gid * 2u * GS_MAXV + gs_ic++] = gid * GS_MAXV + gs_n; gs_n++; gs_open = true; }\n",
               STAGE_SLOTS);
        if (mode & 2) sp(&c->body, "    if (gs_open) { gs_ic++; gs_open = false; }\n");
        if (d != RZ) set_reg(c, d, "gs_n");
    } else if ((w >> 53) == (0xeb0 >> 1) || (w >> 53) == (0xeb2 >> 1) || (w >> 53) == (0xeb4 >> 1) || (w >> 53) == (0xea0 >> 1)) {   // SULD / SUST / SURED / SUATOM (+ .B)
        unsigned top = (unsigned)(w >> 52);
        emit_surface(c, w, top >> 1 == 0xeb0 >> 1 ? SF_SULD : top >> 1 == 0xeb2 >> 1 ? SF_SUST : top >> 1 == 0xeb4 >> 1 ? SF_SURED : SF_SUATOM, top & 1);
    } else if ((w >> 56) == 0xea || (w >> 56) == 0xeb) {   // (RED, 0xebf8, is matched above)
        fail(c, "surface access (opcode %03x: SUATOM.CAS, SUCLAMP-class helpers and other variants) is not translated", (unsigned)(w >> 52));
    } else if (OPM(0x3a00000000000000ull, 0xfe00000000000000ull)) {   // VMNMX: video (byte/half/word) integer min/max, then a second op with Rc
        /* Field layout as Ryujinx's InstVmnmx decoder (MIT; MacNativeGames/vendor/ryujinx InstDecoders.cs): selects A bits 36..38 + sign 48, B 28..30 + sign 49,
         * B from a register when bit 50 else imm16 at 20, MX (max) bit 56, D-format (signed second op) 54, SAT 55, video op 51..53, CC 47.
         * Unlike Ryujinx the mixed-signedness case is computed exactly in 64 bits. Found in Animal Crossing (fragment 23e43e94e45bb1d5 @0x3e8: 3-way max). */
        if (bits(w, 47, 1)) fail(c, "VMNMX.CC is not supported");
        int asel = (int)bits(w, 36, 3), bsel = (int)bits(w, 28, 3), asg = (int)bits(w, 48, 1), bsg = (int)bits(w, 49, 1);
        bool mx = bits(w, 56, 1), dfmt = bits(w, 54, 1), sat = bits(w, 55, 1), bvid = bits(w, 50, 1);
        int vop = (int)bits(w, 51, 3), rc = (int)bits(w, 39, 8);
        char ea[160], eb[160];
#define VSEL(out, src, sel, sg) video_sel(out, src, sel, sg)
        VSEL(ea, R(a), asel, asg);
        if (bvid) VSEL(eb, R((int)bits(w, 20, 8)), bsel, bsg);
        else {
            uint32_t im = (uint32_t)bits(w, 20, 16);
            sprintf(eb, "long(%d)", bsg ? (int)(int16_t)im : (int)im);
        }
#undef VSEL
        bool rsg = asg == bsg ? asg : !mx; /* mixed signedness: a max fits u32, a min fits s32 */
        sp(&c->body, "    { long va = %s, vb = %s; long vr = %s(va, vb);\n", ea, eb, mx ? "max" : "min");
        if (sat && dfmt && !rsg) sp(&c->body, "      vr = min(vr, 2147483647l);\n");
        else if (sat && !dfmt && rsg) sp(&c->body, "      vr = max(vr, 0l);\n");
        video_tail(c, vop, dfmt, rc, d);
    } else {
        fail(c, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
    }
    (void)op13;
    if (guarded) sp(&c->body, "  }\n");
}

// ------------------------------------------------------------------ stages beyond vertex / fragment / compute
// Requirements of the geometry, tessellation and vertex-capture keys; the contract is in maxwell.h and docs/architecture/gfx.md.
static void stage_check(Ctx *c) {
    const ShaderKey *k = c->key;
    int st = c->stage;
    if (st == STAGE_GEOMETRY) {
        if (!k || k->gs_max_vertices == 0) fail(c, "geometry shader without ShaderKey.gs_max_vertices");
        else if (k->gs_max_vertices > 1024) fail(c, "geometry shader declares %u output vertices (the hardware maximum is 1024)", (unsigned)k->gs_max_vertices);
        else if (k->gs_in_vertices && k->gs_in_vertices != 1 && k->gs_in_vertices != 2 && k->gs_in_vertices != 3 && k->gs_in_vertices != 4 && k->gs_in_vertices != 6)
            fail(c, "geometry shader input primitive of %u vertices", (unsigned)k->gs_in_vertices);
        else if (k->gs_out_topology > GS_OUT_LINE_STRIP) fail(c, "geometry shader output topology %u", (unsigned)k->gs_out_topology);
    } else if (st == STAGE_TESS_CTRL || st == STAGE_TESS_EVAL) {
        if (!k || k->tess_out_vertices < 1 || k->tess_out_vertices > 32) fail(c, "tessellation needs ShaderKey.tess_out_vertices in 1..32");
        else if (st == STAGE_TESS_CTRL && (k->tess_in_vertices < 1 || k->tess_in_vertices > 32)) fail(c, "tess control needs ShaderKey.tess_in_vertices in 1..32");
        else if (k->tess_domain == TESS_DOMAIN_ISOLINE) fail(c, "isoline tessellation (Metal has quad and triangle patches only)");
        else if (k->tess_domain > TESS_DOMAIN_ISOLINE) fail(c, "tessellation domain %u", (unsigned)k->tess_domain);
        else if (st == STAGE_TESS_EVAL && (k->xfb_count || k->stage_out))
            fail(c, "transform feedback from a tessellation evaluation shader (the vertex count exists only after tessellation: capture it with a following vertex-only pass)");
    } else if (st == STAGE_FRAGMENT || st == STAGE_COMPUTE) {
        if (k && (k->xfb_count || k->stage_out)) fail(c, "transform feedback / stage-out requested for a %s shader", st == STAGE_FRAGMENT ? "fragment" : "compute");
    }
    if (st == STAGE_GEOMETRY && k && (k->xfb_count || k->stage_out)) fail(c, "transform feedback from a geometry shader is captured by the replay from gs_vtx / gs_idx (same record layout), not in the shader");
    if (k && k->capture_void && (st != STAGE_VERTEX || (!k->xfb_count && !k->stage_out))) fail(c, "capture_void is for a vertex shader with stage_out / transform feedback");
}

// The vertex-stage epilogue that writes the stage-out record and the captured transform feedback components (before the clip-space flips: the values are the
// shader's own). `xo` is the vertex ordinal of the capture: xfb_ctl[0] * instance + (vertex id - xfb_ctl[1]).
static void xfb_epilogue(Ctx *c, Str *e) {
    const ShaderKey *k = c->key;
    if (!k || (!k->xfb_count && !k->stage_out)) return;
    if (k->xfb_count > MAX_XFB_ENTRIES) {
        fail(c, "%u transform feedback entries (maximum %d)", (unsigned)k->xfb_count, MAX_XFB_ENTRIES);
        return;
    }
    sp(e, "  { uint xo = xfb_ctl[0] * iid + (vid - xfb_ctl[1]);\n");
    c->info->uses_xfb = k->xfb_count != 0;
    if (k->stage_out) {
        c->info->uses_stage_out = true;
        c->info->stage_slot_mask |= 1ull << STAGE_SLOT_POS;
        sp(e, "    stage_out[xo * %uu + %d] = out.pos;\n", STAGE_SLOTS, STAGE_SLOT_POS);
        if (c->uses_vsys) {
            c->info->stage_slot_mask |= 1ull << STAGE_SLOT_SYS;
            sp(e, "    stage_out[xo * %uu + %d] = vsys;\n", STAGE_SLOTS, STAGE_SLOT_SYS);
        }
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) {
                c->info->stage_slot_mask |= 1ull << (STAGE_SLOT_GENERIC + i);
                sp(e, "    stage_out[xo * %uu + %d] = out.v%d;\n", STAGE_SLOTS, STAGE_SLOT_GENERIC + i, i);
            }
    }
    for (int n = 0; n < k->xfb_count; n++) {
        const ShaderXfbEntry *x = &k->xfb[n];
        unsigned stride = x->buffer < MAX_XFB_BUFFERS ? k->xfb_stride[x->buffer] : 0;
        if (x->buffer >= MAX_XFB_BUFFERS || x->comps < 1 || x->comps > 4 || (x->addr & 3) || (x->offset & 3) || stride == 0 || (stride & 3) ||
            (unsigned)x->offset + 4u * x->comps > stride) {
            fail(c, "bad transform feedback entry %d (buffer %u, %u components at %#x, offset %u, stride %u)", n, (unsigned)x->buffer, (unsigned)x->comps, (unsigned)x->addr,
                 (unsigned)x->offset, stride);
            return;
        }
        c->info->xfb_buffer_mask |= 1u << x->buffer;
        for (int j = 0; j < x->comps; j++) {
            uint32_t a = x->addr + 4u * (uint32_t)j;
            char v[48];
            int comp = (a >> 2) & 3;
            if (a >= 0x60 && a < 0x70) {
                c->uses_vsys = true;
                sprintf(v, "vsys[%d]", comp);
            }
            else if (a >= 0x70 && a < 0x80) sprintf(v, "out.pos[%d]", comp);
            else if (a >= 0x80 && a < 0x280 && (c->out_attr_mask & (1u << ((a - 0x80) >> 4)))) sprintf(v, "out.v%u[%d]", (a - 0x80) >> 4, comp);
            else strcpy(v, "0.0f");   // an output the program never wrote
            sp(e, "    xfb%d[xfb_ctl[%d] + xo * %uu + %uu] = as_type<uint>(%s);\n", x->buffer, 2 + x->buffer, stride / 4, x->offset / 4 + (unsigned)j, v);
        }
    }
    sp(e, "  }\n");
}

// ------------------------------------------------------------------ program assembly
static bool is_branch_target_op(uint64_t w) { return ((w & 0xfff0000000000020ull) == 0xe240000000000000ull); }

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    Ctx *c = calloc(1, sizeof *c);
    if (c) memset(c->reg_cb, 0xff, sizeof c->reg_cb);
    c->info = info;
    c->key = key;
    c->stage = stage;
    c->ipa_mode = -1;
    const uint8_t *sph = blob + 0x30;
    const uint8_t *code = blob + 0x80;
    // compute programs have neither the 0x30-byte NVN header nor an SPH: code starts at the blob
    if (stage == STAGE_COMPUTE) code = blob;
    if (stage == STAGE_FRAGMENT) {
        c->omap_target = *(const uint32_t *)(sph + 0x48);
        c->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
    }
    // find the end: the last EXIT followed by self-branch padding, or max_size
    size_t n = (max_size - (code - blob)) / 8;
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)code)[i];
        if (i % 4 == 0) continue;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && sbits(w, 20, 24) == -8 && end) break;   // bra self
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    // collect branch targets (labels)
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)code)[i];
        uint32_t pc = (uint32_t)(i * 8);
        bool is_flow = (w & 0xfff0000000000020ull) == 0xe240000000000000ull || (w & 0xfff0000000000020ull) == 0xe290000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull || (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe260000000000000ull;   // CAL: relative like BRA
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000020ull) == 0xe260000000000000ull ||
            (w & 0xfff0000000000020ull) == 0xe220000000000000ull) {   // CAL / JCAL: the return address is a label
            int64_t t = norm_target((int64_t)pc + 8);
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
        }
        if ((w & 0xfff0000000000020ull) == 0xe220000000000000ull || (w & 0xfff0000000000020ull) == 0xe210000000000000ull) {   // JCAL / JMP: absolute target
            int64_t t = norm_target((int64_t)bits(w, 20, 32));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000020ull) == 0xe230000000000000ull || (w & 0xfff0000000000020ull) == 0xe280000000000000ull) {   // PEXIT / PLONGJMP: relative like PBK
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
            if ((w >> 52) == 0xe23) c->has_pexit = true;
            else c->uses_ljs = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe310000000000000ull || (w & 0xfff0000000000000ull) == 0xe2c0000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe2e0000000000000ull) {   // LONGJMP / GETCRSPTR / SETCRSPTR: need the state machine's stacks
            any_branch = true;
            if ((w >> 52) == 0xe31) c->uses_ljs = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe320000000000000ull) any_branch = true;   // RET
        if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull ||
            (w & 0xfff0000000000020ull) == 0xe200000000000000ull) {   // BRX / JMX: any instruction may be a target
            any_branch = true;
            memset(label, 1, end);
        }
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || (w & 0xfff0000000000000ull) == 0xe340000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe350000000000000ull)
            any_branch = true;
        (void)is_branch_target_op;
    }
    // translate
    static int annotate = -1;
    if (annotate < 0) annotate = getenv("NAPPLE_SAVE_MSL") != NULL;
    c->branchy = any_branch;
    for (size_t i = 0; i < end && !c->fail; i++) {
        if (i % 4 == 0) continue;
        uint32_t pc = (uint32_t)(i * 8);
        if (any_branch && label[i]) sp(&c->body, "  case %uu:\n", pc);
        if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)((const uint64_t *)code)[i]);
        emit_insn(c, ((const uint64_t *)code)[i], pc);
        info->num_insts++;
        if (key && key->probe_pc && key->probe_pc == pc && stage == STAGE_FRAGMENT) /* NAPPLE_SHADER_PROBE: show intermediate registers as the colour */
            sp(&c->body,
               "    r0 = as_type<uint>(as_type<float>(r%u) * %.9gf); r1 = as_type<uint>(as_type<float>(r%u) * %.9gf); r2 = as_type<uint>(as_type<float>(r%u) * %.9gf); r3 = 0x3f800000u; exit_now = true; break;\n",
               key->probe_reg[0], key->probe_scale, key->probe_reg[1], key->probe_scale, key->probe_reg[2], key->probe_scale);
    }
    free(label);
    if (c->uses_smem && !c->fail) {
        uint32_t sz = c->smem_dyn ? 32768u : c->smem_need;
        if (key && key->shared_size && (c->smem_dyn || key->shared_size > sz)) sz = key->shared_size;
        sz = (sz + 15u) & ~15u;
        if (sz == 0) sz = 16;
        if (sz > 32768u) fail(c, "shared memory of %u bytes exceeds Metal's 32 KB threadgroup limit", sz);
        info->shared_mem = sz;
    }
    for (int i = 0; i < info->ntex && !c->fail; i++) {
        const ShaderTexture *t = &info->tex[i];
        if (!t->stencil) continue;
        if (t->type != TEXTYPE_2D && t->type != TEXTYPE_2D_ARRAY) fail(c, "stencil sampling of a texture of type %d (only 2D and 2D array stencil views exist)", t->type);
        else if (t->shadow) fail(c, "depth comparison on a stencil texture binding");
        else c->uses_stencil = true;
    }
    stage_check(c);
    if (c->fail) {
        free(c->body.buf);
        free(c);
        return info;
    }
    Str xfb_epi = {0};
    if (stage == STAGE_VERTEX) xfb_epilogue(c, &xfb_epi);
    if (c->fail) {
        free(c->body.buf);
        free(xfb_epi.buf);
        free(c);
        return info;
    }

    // ---------------------------------------------------------- MSL source
    Str s = {0};
    sp(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    sp(&s, "static inline uint cbr(constant uint4 *cb, uint sz, uint off) { return off < sz ? cb[off >> 4][(off >> 2) & 3] : 0u; }\n");
    sp(&s,
       "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n");
    sp(&s,
       "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n");
    sp(&s,
       "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n");
    sp(&s,
       "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n");
    sp(&s, "static inline uint2 rsc(uint2 c, float4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uint2(floor((float2(c) + 0.5f) * s.xy)); }\n");
    sp(&s,
       "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n");
    // merges on half2 lanes rather than with integer masks: on an M1 (macOS 15.0.1) the Metal compiler miscompiled
    // (old & 0xFFFF0000u) | lo in Link's body shader - the merged low half read back as a constant 512.0 and Link
    // rendered white. Same bits either way.
    sp(&s,
       "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); half2 o = as_type<half2>(old), n = half2(r); if (merge == 2u) o.x = n.x; else if (merge == 3u) o.y = n.y; else o = n; return as_type<uint>(o); }\n");
    sp(&s,
       "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint n = (sel >> (i * 4)) & 15u; uint s = n & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; if (n & 8u) byte = (byte & 0x80u) ? 0xFFu : 0u; r |= byte << (i * 8); } return r; }\n");
    /* PRMT modes: the selector is the low two bits of the selector operand, expanded to the default nibble form (F4E, B4E, RC8, ECL, ECR, RC16) */
    sp(&s,
       "static inline uint prmt_m(uint a, uint b, uint sel, uint mode) { uint c = sel & 3u; uint n = 0; for (uint i = 0; i < 4; i++) { uint v; "
       "switch (mode) { case 1u: v = c + i; break; case 2u: v = 4u + ((c + 4u - i) & 3u); break; case 3u: v = c; break; case 4u: v = i <= c ? i : 3u; break; case 5u: v = min(i, c); break; default: v = (c & 1u) * 2u + (i & 1u); break; } "
       "n |= (v & 7u) << (i * 4); } return prmt(a, b, n); }\n");

    if (c->uses_f64) {   // one newline per function keeps the generated text readable and the Metal diagnostics locatable
        sp(&s, "// ---- IEEE 754 binary64 emulation (soft_double_core.inc)\n");
        for (const char *q = sd_msl_source; *q; q++) {
            sp(&s, "%c", *q);
            if (*q == '}' && q[1] == ' ') sp(&s, "\n");
        }
        sp(&s, "\n");
    }
    if (c->atom_dev || c->atom_tg) {
        /* integer atomics; op: 0 add, 1 min, 2 max, 3 inc (wraps at the operand), 4 dec (wraps to the operand), 5 and, 6 or, 7 xor, 8 exchange. All return the old value. */
        static const char *const as[2][2] = {{"atom_dev", "device"}, {"atom_tg", "threadgroup"}};
        for (int k = 0; k < 2; k++) {
            if (!(k == 0 ? c->atom_dev : c->atom_tg)) continue;
            sp(&s,
               "static inline uint %s(%s atomic_uint *p, uint op, uint v, bool sgn) {\n  switch (op) {\n"
               "  case 0u: return atomic_fetch_add_explicit(p, v, memory_order_relaxed);\n  case 5u: return atomic_fetch_and_explicit(p, v, memory_order_relaxed);\n"
               "  case 6u: return atomic_fetch_or_explicit(p, v, memory_order_relaxed);\n  case 7u: return atomic_fetch_xor_explicit(p, v, memory_order_relaxed);\n"
               "  case 8u: return atomic_exchange_explicit(p, v, memory_order_relaxed);\n  default: break;\n  }\n"
               "  uint old = atomic_load_explicit(p, memory_order_relaxed);\n  for (;;) {\n    uint nv;\n"
               "    if (op == 1u) nv = sgn ? uint(min(int(old), int(v))) : min(old, v);\n    else if (op == 2u) nv = sgn ? uint(max(int(old), int(v))) : max(old, v);\n"
               "    else if (op == 3u) nv = old >= v ? 0u : old + 1u;\n    else nv = (old == 0u || old > v) ? v : old - 1u;\n"
               "    if (atomic_compare_exchange_weak_explicit(p, &old, nv, memory_order_relaxed, memory_order_relaxed)) return old;\n  }\n}\n",
               as[k][0], as[k][1]);
        }
    }
    if (c->uses_smem) sp(&s, "constant uint SMEM_BYTES = %uu;\n", info->shared_mem);
    if (c->uses_vabsdiff4)
        sp(&s,
           "static inline uint vabsdiff4(uint a, uint b, uint c, bool sa, bool sb, uint op, bool sat) {\n  uint r = 0u, sum = 0u;\n"
           "  for (uint i = 0u; i < 4u; i++) {\n    int x = sa ? int(char(a >> (i * 8u))) : int((a >> (i * 8u)) & 0xFFu);\n"
           "    int y = sb ? int(char(b >> (i * 8u))) : int((b >> (i * 8u)) & 0xFFu);\n    uint dl = uint(abs(x - y)), cl = (c >> (i * 8u)) & 0xFFu;\n"
           "    sum += dl;\n    r |= ((op == 2u ? min(dl, cl) : (op == 3u ? max(dl, cl) : dl)) & 0xFFu) << (i * 8u);\n  }\n"
           "  if (op == 1u) { uint t = sum + c; return (sat && t < sum) ? 0xFFFFFFFFu : t; }\n  return r;\n}\n");
    if (c->uses_stencil) {
        /* Stencil views are uint textures (X32_Stencil8 / Stencil8): Metal cannot `sample` an integer texture, so the shader's texture name is a wrapper with the
         * float-texture member functions the translator emits. Sampling is a nearest read of level 0 (stencil values are never filtered; bias / lod / offset
         * / gradient arguments are ignored). The result is the integer in x with (0, 0, 1) behind it, carried as raw bits in a float4 like every register. */
        sp(&s, "static inline float4 stx(uint v) { return as_type<float4>(uint4(v, 0u, 0u, 1u)); }\n");
        sp(&s,
           "struct StencilTex2D { texture2d<uint> t;\n"
           "  uint get_width(uint l = 0u) const { return t.get_width(l); } uint get_height(uint l = 0u) const { return t.get_height(l); }\n"
           "  uint get_num_mip_levels() const { return t.get_num_mip_levels(); }\n"
           "  float2 calculate_clamped_lod(sampler, float2) const { return float2(0.0f); } float2 calculate_unclamped_lod(sampler, float2) const { return float2(0.0f); }\n"
           "  float4 read(uint2 c, uint l = 0u) const { return stx(t.read(c, l).x); }\n"
           "  uint at(int2 p) const { return t.read(uint2(clamp(p, int2(0), int2(int(t.get_width()) - 1, int(t.get_height()) - 1)))).x; }\n"
           "  template <typename... A> float4 sample(sampler, float2 uv, A...) const { return stx(at(int2(floor(uv * float2(t.get_width(), t.get_height()))))); }\n"
           "  template <typename... A> float4 gather(sampler, float2 uv, A...) const { int2 b = int2(floor(uv * float2(t.get_width(), t.get_height()) - 0.5f));\n"
           "    return as_type<float4>(uint4(at(b + int2(0, 1)), at(b + int2(1, 1)), at(b + int2(1, 0)), at(b))); } };\n");
        sp(&s,
           "struct StencilTex2DArray { texture2d_array<uint> t;\n"
           "  uint get_width(uint l = 0u) const { return t.get_width(l); } uint get_height(uint l = 0u) const { return t.get_height(l); }\n"
           "  uint get_array_size() const { return t.get_array_size(); } uint get_num_mip_levels() const { return t.get_num_mip_levels(); }\n"
           "  float2 calculate_clamped_lod(sampler, float2) const { return float2(0.0f); } float2 calculate_unclamped_lod(sampler, float2) const { return float2(0.0f); }\n"
           "  float4 read(uint2 c, uint layer, uint l = 0u) const { return stx(t.read(c, layer, l).x); }\n"
           "  uint at(int2 p, uint layer) const { return t.read(uint2(clamp(p, int2(0), int2(int(t.get_width()) - 1, int(t.get_height()) - 1))), layer).x; }\n"
           "  template <typename... A> float4 sample(sampler, float2 uv, uint layer, A...) const { return stx(at(int2(floor(uv * float2(t.get_width(), t.get_height()))), layer)); }\n"
           "  template <typename... A> float4 gather(sampler, float2 uv, uint layer, A...) const { int2 b = int2(floor(uv * float2(t.get_width(), t.get_height()) - 0.5f));\n"
           "    return as_type<float4>(uint4(at(b + int2(0, 1), layer), at(b + int2(1, 1), layer), at(b + int2(1, 0), layer), at(b, layer))); } };\n");
    }
    if (c->uses_idp)
        sp(&s,
           "static inline uint idp(uint a, uint b, uint c, bool asg, bool bsg, bool two, bool hi) {\n"
           "  int acc = 0; uint n = two ? 2u : 4u;\n"
           "  for (uint i = 0u; i < n; i++) {\n"
           "    int x = two ? (asg ? int(short(a >> (16u * i))) : int((a >> (16u * i)) & 0xFFFFu)) : (asg ? int(char(a >> (8u * i))) : int((a >> (8u * i)) & 0xFFu));\n"
           "    uint bs = two ? (hi ? 16u : 0u) + 8u * i : 8u * i;\n"
           "    int y = bsg ? int(char(b >> bs)) : int((b >> bs) & 0xFFu);\n"
           "    acc += x * y;\n  }\n  return uint(acc) + c;\n}\n");
    if (c->uses_isel)
        sp(&s,
           "static inline uint isel(uint v, uint s) { switch (s) { case 2u: return v & 0xFFu; case 3u: return uint(int(char(v))); case 4u: return v & 0xFFFFu; "
           "case 5u: return uint(int(short(v))); default: return v; } }\n");
    if (stage == STAGE_GEOMETRY) {
        /* the output vertex copy: only the slots the program stores; the position gets the clip-space conversions the vertex stage would apply */
        sp(&s, "constant uint GS_MAXV = %uu;\n", (unsigned)key->gs_max_vertices);
        sp(&s, "static inline void gs_put(device float4 *dst, thread const float4 *src) {\n");
        info->stage_slot_mask |= c->gs_slots | (1ull << STAGE_SLOT_POS);
        for (int i = 0; i < STAGE_SLOTS; i++) {
            if (!(info->stage_slot_mask & (1ull << i))) continue;
            if (i == STAGE_SLOT_POS) {
                sp(&s, "  float4 p = src[%d];\n", i);
                if (key->flip_y) sp(&s, "  p.y = -p.y;\n");
                if (key->depth_minus_one) sp(&s, "  p.z = (p.z + p.w) * 0.5f;\n");
                sp(&s, "  dst[%d] = p;\n", i);
            } else sp(&s, "  dst[%d] = src[%d];\n", i, i);
        }
        sp(&s, "}\n");
        info->gs_max_vertices = key->gs_max_vertices;
        info->gs_out_topology = key->gs_out_topology;
    }
    if (stage == STAGE_TESS_CTRL || stage == STAGE_TESS_EVAL) {
        info->tess_domain = key->tess_domain;
        info->tess_out_vertices = key->tess_out_vertices;
        info->stage_slot_mask |= c->gs_slots;
        info->patch_slot_mask |= c->patch_slots;
    }

    // inputs/outputs
    if (stage == STAGE_VERTEX || stage == STAGE_TESS_EVAL) {
        if (stage == STAGE_VERTEX) {
            sp(&s, "struct VIn {\n");
            for (int i = 0; i < 32; i++) {
                if (!(c->in_attr_mask & (1u << i))) continue;
                int kind = key ? key->attr_kind[i] : 0;
                sp(&s, "  %s4 a%d [[attribute(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", i, i);
            }
            sp(&s, "};\n");
        }
        sp(&s, "struct VOut {\n  float4 pos [[position, invariant]];\n");   // depth pre-pass + EQUAL test needs identical positions
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  float4 v%d [[user(locn%d)]];\n", i, i);
        if (info->writes_point_size) sp(&s, "  float psize [[point_size]];\n");
        sp(&s, "};\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct FIn {\n  float4 pos [[position]];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
            sp(&s, "  float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
        }
        sp(&s, "};\n");
        sp(&s, "struct FOut {\n");
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "  %s4 c%d [[color(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", rt, rt);
        }
        if (c->omap_depth) {
            sp(&s, "  float depth [[depth(any)]];\n");
            info->writes_depth = true;
        }
        sp(&s, "};\n");
    }

    // entry point
    Str args = {0};
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask & (1u << i)) sp(&args, ", constant uint4 *cb%d [[buffer(%d)]]", i, i);
    if (info->cbuf_dynamic)
        sp(&args, ", constant uint *cbsz [[buffer(%d)]]", CBSZ_BUFFER_INDEX); /* bound sizes: a read past the end returns 0 like the hardware */
    if (info->uses_gmem) {
        if (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX)) {
            free(args.buf);
            args = (Str){0};   // (was a use after free + double free of args.buf)
            fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
        }
        if (maxwell_gmem_size != ~0ull) sp(&args, ", device uchar *gmem [[buffer(%d)]]", GMEM_BUFFER_INDEX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&args, ", constant float4 *rscale [[buffer(18)]]");
        info->uses_rscale = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        const char *tn = tex_type_name(t->type);
        /* Metal allows 16 samplers per function: textures from the 17th on share the first texture's sampler */
        if (t->stencil) sp(&args, ", texture%s<uint> t%d_u [[texture(%d)]]", tn, i, i);
        else if (t->shadow || t->depth) sp(&args, ", depth%s<float> t%d [[texture(%d)]]", tn, i, i);
        else sp(&args, ", texture%s<float> t%d [[texture(%d)]]", tn, i, i);
        if (i < 16) sp(&args, ", sampler s%d [[sampler(%d)]]", i, i);
    }
    emit_image_args(&args, info);
    if (stage == STAGE_VERTEX && key && (key->xfb_count || key->stage_out)) {
        sp(&args, ", constant uint *xfb_ctl [[buffer(%d)]]", XFB_CTL_BUFFER_INDEX);
        if (key->stage_out) sp(&args, ", device float4 *stage_out [[buffer(%d)]]", STAGE_OUT_BUFFER_INDEX);
        for (int k = 0; k < MAX_XFB_BUFFERS; k++)
            if (info->xfb_buffer_mask & (1u << k)) sp(&args, ", device uint *xfb%d [[buffer(%d)]]", k, XFB_BUFFER_BASE + k);
    }
    if (stage == STAGE_VERTEX) {
        /* capture_void: a capture-only pass (stage_out / xfb) returns nothing, so the pipeline may disable rasterization (Metal demands a void vertex function for that) */
        sp(&s, "vertex %s main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n", key && key->capture_void ? "void" : "VOut", "", args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "fragment FOut main0(FIn in [[stage_in]], bool front_facing [[front_facing]]%s%s%s%s) {\n",
           c->uses_lane ? ", uint lane [[thread_index_in_simdgroup]]" : "", c->uses_point_coord ? ", float2 point_coord [[point_coord]]" : "",
           c->key && c->key->sample_shading ? ", uint sample_id [[sample_id]]" : "" /* declared = Metal runs the function per sample (NVN sample shading) */,
           args.buf ? args.buf : "");
    } else if (stage == STAGE_GEOMETRY) {
        sp(&s,
           "kernel void main0(uint gid [[thread_position_in_grid]], constant uint *sp [[buffer(%d)]], device const float4 *stage_in [[buffer(%d)]], "
           "device float4 *gs_vtx [[buffer(%d)]], device uint *gs_idx [[buffer(%d)]]%s) {\n",
           STAGE_PARAM_BUFFER_INDEX, STAGE_IN_BUFFER_INDEX, GS_VERTEX_BUFFER_INDEX, GS_INDEX_BUFFER_INDEX, args.buf ? args.buf : "");
        /* GS instancing: sp[3] invocations per input primitive (0 / 1 = one). Thread gid = primitive gs_pid, invocation inv; every thread owns its own
         * gs_vtx / gs_idx slots (the replay sizes them per thread, output record index = gid * GS_MAXV + n). */
        sp(&s, "  uint gs_ninv = max(sp[3], 1u);\n  if (gid >= sp[0] * gs_ninv) return;\n  uint gs_pid = gid / gs_ninv, inv = gid - gs_pid * gs_ninv;\n  float4 gso[%d];\n"
               "  for (uint i = 0u; i < %uu; i++) gso[i] = float4(0.0f, 0.0f, 0.0f, 1.0f);\n", STAGE_SLOTS, STAGE_SLOTS);
        sp(&s, "  for (uint i = 0u; i < 2u * GS_MAXV; i++) gs_idx[gid * 2u * GS_MAXV + i] = 0xFFFFFFFFu;\n  uint gs_n = 0u, gs_ic = 0u;\n  bool gs_open = false;\n");
    } else if (stage == STAGE_TESS_CTRL) {
        sp(&s,
           "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]], constant uint *sp [[buffer(%d)]], "
           "device const float4 *stage_in [[buffer(%d)]], device float4 *tess_vtx [[buffer(%d)]], device float4 *tess_patch [[buffer(%d)]], "
           "device half *tess_fac [[buffer(%d)]]%s) {\n",
           STAGE_PARAM_BUFFER_INDEX, STAGE_IN_BUFFER_INDEX, TESS_VERTEX_BUFFER_INDEX, TESS_PATCH_BUFFER_INDEX, TESS_FACTOR_BUFFER_INDEX, args.buf ? args.buf : "");
        sp(&s, "  uint inv = tid.x, pid = ctaid.x;\n  if (pid >= sp[0]) return;\n");
    } else if (stage == STAGE_TESS_EVAL) {
        sp(&s,
           "[[patch(%s, %u)]] vertex VOut main0(uint patch_id [[patch_id]], %s tess_coord [[position_in_patch]], constant uint *sp [[buffer(%d)]], "
           "device const float4 *tess_vtx [[buffer(%d)]], device const float4 *tess_patch [[buffer(%d)]]%s) {\n",
           key->tess_domain == TESS_DOMAIN_TRIANGLE ? "triangle" : "quad", (unsigned)key->tess_out_vertices, key->tess_domain == TESS_DOMAIN_TRIANGLE ? "float3" : "float2",
           STAGE_PARAM_BUFFER_INDEX, TESS_VERTEX_BUFFER_INDEX, TESS_PATCH_BUFFER_INDEX, args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else {
        sp(&s, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]]%s) {\n", args.buf ? args.buf : "");
    }
    free(args.buf);
    if (c->uses_smem) sp(&s, "  threadgroup uint smem_w[%u];\n  threadgroup uchar *smem = (threadgroup uchar *)smem_w;\n", info->shared_mem / 4);
    for (int i = 16; i < info->ntex; i++) sp(&s, "  sampler s%d = s0;\n", i);
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_attr_arr) {
        sp(&s, "  float attr_arr[128];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) {
                sp(&s, "  for (uint k = 0; k < 4; k++) attr_arr[%d + k] = 0.0f;\n", i * 4);
                continue;
            }
            sp(&s, "  for (uint k = 0; k < 4; k++) attr_arr[%d + k] = in.v%d[k];\n", i * 4, i);
        }
    }
    if (c->uses_vattr_arr) {
        sp(&s, "  uint vattr_arr[128];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) {
                sp(&s, "  for (uint k = 0; k < 4; k++) vattr_arr[%d + k] = 0u;\n", i * 4);
                continue;
            }
            sp(&s, "  for (uint k = 0; k < 4; k++) vattr_arr[%d + k] = as_type<uint>(in.a%d[k]);\n", i * 4, i);
        }
    }
    if (c->uses_vsys) sp(&s, "  float4 vsys = float4(0.0f);\n");
    if (c->uses_cc) sp(&s, "  uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d] = {};\n", LMEM_WORDS);
    if (c->uses_lmb) sp(&s, "  uint lmb = 0u;\n");
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].stencil) sp(&s, "  const StencilTex%s t%d = {t%d_u};\n", info->tex[i].type == TEXTYPE_2D_ARRAY ? "2DArray" : "2D", i, i);
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0; uint pbk[16]; uint pbk_n = 0; uint pcnt[16]; uint pcnt_n = 0; uint cal[16]; uint cal_n = 0;\n");
        if (c->uses_ljs) sp(&s, "  uint ljs[16]; uint ljs_n = 0;\n");
        if (c->has_pexit) sp(&s, "  uint pex = 0xFFFFFFFFu;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", c->body.buf ? c->body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", c->body.buf ? c->body.buf : "");
    }
    if (stage == STAGE_VERTEX || stage == STAGE_TESS_EVAL) {
        if (xfb_epi.buf) sp(&s, "%s", xfb_epi.buf);
        if (key && key->flip_y) sp(&s, "  out.pos.y = -out.pos.y;\n");
        if (key && key->depth_minus_one) sp(&s, "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        if (stage == STAGE_VERTEX && key && key->capture_void) sp(&s, "  (void)out;\n}\n");
        else sp(&s, "  return out;\n}\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "  FOut o;\n");
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  o.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                    else sp(&s, "  o.c%d[%d] = r%d;\n", rt, comp, r);
                } else {
                    sp(&s, "  o.c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  o.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "  o.depth = in.pos.z;\n");
        }
        sp(&s, "  return o;\n}\n");
    } else if (stage == STAGE_TESS_CTRL) {
        /* the factors the tessellator reads: written from the patch record's level slots once every invocation has stored its share */
        sp(&s, "  threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup);\n");
        if (c->patch_slots & 3ull) {
            const char *dom = key->tess_domain == TESS_DOMAIN_TRIANGLE ? "triangle" : "quad";
            if (key->tess_domain == TESS_DOMAIN_TRIANGLE)
                sp(&s,
                   "  if (inv == 0u) { // %s: edge[3], inside[1] halves per patch\n    for (uint i = 0u; i < 3u; i++) tess_fac[pid * 4u + i] = half(tess_patch[pid * %uu + 0u][i]);\n"
                   "    tess_fac[pid * 4u + 3u] = half(tess_patch[pid * %uu + 1u][0]);\n  }\n",
                   dom, STAGE_SLOTS, STAGE_SLOTS);
            else
                sp(&s,
                   "  if (inv == 0u) { // %s: edge[4], inside[2] halves per patch\n    for (uint i = 0u; i < 4u; i++) tess_fac[pid * 6u + i] = half(tess_patch[pid * %uu + 0u][i]);\n"
                   "    for (uint i = 0u; i < 2u; i++) tess_fac[pid * 6u + 4u + i] = half(tess_patch[pid * %uu + 1u][i]);\n  }\n",
                   dom, STAGE_SLOTS, STAGE_SLOTS);
        }
        sp(&s, "}\n");
    } else {
        sp(&s, "  return;\n}\n");
    }
    free(xfb_epi.buf);
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    info->indexed_inputs = c->uses_attr_arr;
    free(c->body.buf);
    free(c);
    return info;
}

void maxwell_free(ShaderInfo *s) {
    if (!s) return;
    free(s->msl);
    free(s);
}
