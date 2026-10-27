/* a32lift: AArch32 -> AArch64 ahead-of-time lifter (see a32lift.h). Portable C; the decoder is the interpreter's (src/a32/a32_decode_*.c), so the
 * lifter and the interpreter agree on what every encoding means.
 *
 * Register convention of the generated code:
 *   w0..w14  guest r0..r14             x19 instruction budget (negative = leave)     x20 guest window base
 *   NZCV     guest N Z C V             x21 A32Cpu *                                   x22 hash table (guest key -> block)
 *   w15,w16,w17,w24..w28 temporaries (w24 = indirect target / exit pc, w25 = exit reason)   x30 link register of the fallback stub */
#include "a32lift.h"
#include "../a32/a32_priv.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { F_ENT = 1, F_CHK = 2, F_VIS = 4 };
#define CHUNK_SHIFT 15
#define EXIT_NOBLOCK 100

typedef struct {
    uint8_t *fa, *ft;
    uint32_t base, text_end;
} ModFlags;

typedef struct {
    const LiftInput *in;
    ModFlags *mf;
    uint32_t *stack;
    size_t nstack, capstack;
    FILE *o;
    unsigned label;
    uint32_t cur_chunk;
    LiftStats *st;
    const char *prefix;
    char **files;
    unsigned nfiles, capfiles;
    /* per-instruction emission context */
    uint32_t pc;
    int thumb;
    int fprun;  /* floating point run open in the emitted code: 0 none, 1 NEON (standard FPSCR), 2 VFP (guest FPSCR modes) */
    FILE *pool; /* read-only data of the region being emitted: the decoded instructions the out-of-line operations are passed */
    char *poolbuf;
    size_t poolsz;
} Lift;

static const char *const W[16] = {"w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7", "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15"};
static const char *const X[16] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15"};
static const char *const CC[16] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "al"};

static void out(Lift *L, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(Lift *L, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(L->o, fmt, ap);
    fputc('\n', L->o);
    va_end(ap);
}

/* ---- guest memory and flag tables ---------------------------------------------------------------------------------- */
static uint8_t *flagref(Lift *L, uint32_t addr, int thumb) {
    for (unsigned m = 0; m < L->in->nmods; m++) {
        ModFlags *f = &L->mf[m];
        if (addr >= f->base && addr - f->base < f->text_end) return thumb ? &f->ft[(addr - f->base) >> 1] : &f->fa[(addr - f->base) >> 2];
    }
    return NULL;
}
static uint32_t ld32g(const Lift *L, uint32_t a) {
    uint32_t v;
    memcpy(&v, L->in->mem + a, 4);
    return v;
}
static uint32_t ld16g(const Lift *L, uint32_t a) {
    uint16_t v;
    memcpy(&v, L->in->mem + a, 2);
    return v;
}

/* Decodes the instruction at addr; returns its length (0 when outside the text). */
static unsigned decode_at(Lift *L, uint32_t addr, int thumb, A32Insn *i) {
    if (!flagref(L, addr, thumb)) return 0;
    memset(i, 0, sizeof *i);
    if (thumb) {
        uint16_t h1 = (uint16_t)ld16g(L, addr), h2 = a32_thumb_is32(h1) ? (uint16_t)ld16g(L, addr + 2) : 0xffff;
        return a32_decode_thumb(h1, h2, i);
    }
    if (addr & 3) return 0;
    a32_decode_arm(ld32g(L, addr), i);
    return 4;
}
/* The interpreter's rule for the condition of an instruction (Thumb: the IT state decides when the encoding itself is unconditional). */
static unsigned insn_cond(const A32Insn *i, int thumb, unsigned it) {
    unsigned c = i->cond != 15 ? i->cond : (thumb && it ? it >> 4 : 15);
    return c >= 14 ? 15 : c;
}
static unsigned it_advance(unsigned it) { return (it & 7) == 0 ? 0 : (it & 0xe0) | ((it << 1) & 0x1f); }

/* ---- discovery ------------------------------------------------------------------------------------------------------ */
static void push(Lift *L, uint32_t addr, int thumb) {
    if (L->nstack == L->capstack) {
        L->capstack = L->capstack ? L->capstack * 2 : 4096;
        L->stack = realloc(L->stack, L->capstack * sizeof *L->stack);
    }
    L->stack[L->nstack++] = addr | (thumb ? 1u : 0u);
}
static void add_entry(Lift *L, uint32_t addr, int thumb, int chk) {
    uint8_t *f = flagref(L, addr, thumb);
    if (!f) return;
    *f |= F_ENT | (chk ? F_CHK : 0);
    if (!(*f & F_VIS)) push(L, addr, thumb);
}

#define RECENT 32
typedef struct {
    uint32_t addr;
    A32Insn i;
} Recent;

static void discover_tb(Lift *L, const A32Insn *i, uint32_t pcv, const Recent *rec, unsigned nrec) {
    unsigned n = 0;
    for (unsigned k = 0; k < nrec; k++) { /* cmp rX, #N shortly before: N + 1 table entries */
        const A32Insn *p = &rec[nrec - 1 - k].i;
        if (p->op == OP_DP && p->sub == 10 && p->k == K_IMM && p->rn == i->rm && p->imm < 512) {
            n = p->imm + 1;
            break;
        }
    }
    if (!n) n = 16;
    for (unsigned e = 0; e < n; e++) {
        uint32_t off = i->sub == 0 ? (uint32_t)L->in->mem[pcv + e] : ld16g(L, pcv + 2 * e);
        uint32_t t = pcv + off * 2;
        if (flagref(L, t, 1)) add_entry(L, t, 1, 1);
    }
}

/* Compiler switch tables of the form `adr rB, table; ldr rT, [rI, rB]; add pc, rT, rB` (entries are offsets from the table): the entries are code. The count
 * comes from a preceding `cmp rI, #N` (N + 1 entries); without one the table is read until an entry stops looking like code. */
static void discover_offset_table(Lift *L, const A32Insn *i, uint32_t addr, const Recent *rec, unsigned nrec) {
    uint32_t tab = 0;
    int found = 0;
    for (unsigned k = 0; k < nrec && !found; k++) {
        const Recent *p = &rec[nrec - 1 - k];
        if (p->i.op == OP_DP && p->i.rd == i->rm && p->i.rn == 15 && p->i.k == K_IMM && (p->i.sub == 4 || p->i.sub == 2)) {
            uint32_t pcv = p->addr + 8;
            tab = p->i.sub == 4 ? pcv + p->i.imm : pcv - p->i.imm;
            found = 1;
        }
    }
    if (!found) { /* the base register was set up too far back: the table usually follows the branch directly */
        tab = addr + 4;
        found = 1;
    }
    unsigned n = 0;
    for (unsigned k = 0; k < nrec; k++) {
        const A32Insn *p = &rec[nrec - 1 - k].i;
        if (p->op == OP_DP && p->sub == 10 && p->k == K_IMM && p->imm < 512) {
            n = p->imm + 1;
            break;
        }
    }
    (void)addr;
    for (unsigned e = 0; e < (n ? n : 512); e++) {
        uint32_t t = tab + ld32g(L, tab + 4 * e);
        if (!flagref(L, t, 0) || (t & 3)) {
            if (n) continue;
            break;
        }
        add_entry(L, t, 0, 1);
    }
}

/* Jumps into the middle of an unrolled sequence (the libgcc/compiler-rt division routines): `add ip, pc, #n; sub ip, ip, rK, lsl #a; [sub ip, ip, rK, lsl #b;] bx ip`.
 * Every instruction position reachable by the subtraction is an entry: base - j * stride, down to the bx. */
static void discover_computed_offset(Lift *L, const A32Insn *i, uint32_t addr, const Recent *rec, unsigned nrec) {
    unsigned reg = i->rm, stride = 0, ridx = 16;
    uint32_t base = 0;
    int have_base = 0;
    for (unsigned k = 0; k < nrec; k++) {
        const Recent *p = &rec[nrec - 1 - k];
        if (p->i.op != OP_DP || p->i.rd != reg) continue;
        if (p->i.sub == 2 && p->i.rn == reg && p->i.k == K_RSI && p->i.sh == 0 && p->i.sa < 8 && (ridx == 16 || ridx == p->i.rm)) {
            ridx = p->i.rm;
            stride += 1u << p->i.sa;
        } else if (p->i.sub == 4 && p->i.rn == 15 && p->i.k == K_IMM) {
            base = p->addr + 8 + p->i.imm;
            have_base = 1;
            break;
        } else break;
    }
    if (!have_base || !stride) return;
    for (uint32_t t = base; t > addr && base - t < 0x2000; t -= stride) {
        if (!flagref(L, t, 0) || (t & 3)) break;
        add_entry(L, t, 0, 1);
    }
}

/* Position-independent code addresses: `ldr rX, [pc, #lit]; add rX, rX, pc` (or `add rY, pc, rX`) builds a code address from a literal. */
static void discover_pc_relative(Lift *L, const A32Insn *i, uint32_t addr, int thumb, const Recent *rec, unsigned nrec) {
    unsigned x;
    if (i->rm == 15 && i->rn != 15) x = i->rn;
    else if (i->rn == 15 && i->rm != 15) x = i->rm;
    else return;
    for (unsigned k = 0; k < nrec; k++) {
        const Recent *p = &rec[nrec - 1 - k];
        if (p->i.op != OP_DP && p->i.op != OP_LDST) continue;
        if (p->i.op == OP_DP && p->i.sub >= 8 && p->i.sub <= 11) continue;                                      /* cmp / cmn / tst / teq write no register */
        if (p->i.op == OP_LDST && (p->i.sub == 0 || p->i.sub == 2 || p->i.sub == 4 || p->i.sub == 8)) continue; /* stores */
        if (p->i.rd == x && p->i.op == OP_LDST && p->i.sub == 1 && p->i.rn == 15 && !(p->i.fl & FL_REG)) {
            uint32_t pv = (p->addr + (thumb ? 4 : 8)) & ~3u, la = (p->i.fl & FL_U) ? pv + p->i.imm : pv - p->i.imm;
            if (!flagref(L, la, 0) && (la < 0x200000 || la > 0xf0000000u)) return;
            uint32_t t = ld32g(L, la) + addr + (thumb ? 4 : 8);
            if (flagref(L, t & ~1u, t & 1) && ((t & 1) || !(t & 3))) add_entry(L, t & ~1u, t & 1, 1);
            return;
        }
        if (p->i.rd == x) return; /* the register was overwritten by something else */
    }
}

/* push {..., lr} (ARM stmdb sp!, Thumb push / stmdb sp!) or the common `mov ip, sp` / `sub sp` openers: the first instruction of a function. */
static int looks_like_prologue(Lift *L, uint32_t a, int thumb) {
    if (!flagref(L, a, thumb)) return 0;
    if (thumb) {
        uint32_t h = ld16g(L, a);
        if ((h & 0xff00) == 0xb500) return 1;                    /* push {..., lr} */
        if (h == 0xe92d && (ld16g(L, a + 2) & 0x4000)) return 1; /* stmdb sp!, {..., lr} */
        return 0;
    }
    if (a & 3) return 0;
    uint32_t w = ld32g(L, a);
    return (w & 0xffff4000u) == 0xe92d4000u || w == 0xe1a0c00du || (w & 0xfffff000u) == 0xe24dd000u ||
           w == 0xe52de004u; /* push {..lr}; mov ip, sp; sub sp, sp, #n; str lr, [sp, #-4]! */
}

static void walk_run(Lift *L, uint32_t start, int thumb) {
    unsigned it = 0;
    Recent rec[RECENT];
    unsigned nrec = 0;
    uint32_t addr = start;
    for (;;) {
        uint8_t *f = flagref(L, addr, thumb);
        if (!f) break;
        if (*f & F_VIS) {
            if (addr != start) *f |= F_ENT;
            break;
        }
        *f |= F_VIS;
        A32Insn i;
        unsigned len = decode_at(L, addr, thumb, &i);
        if (!len) break;
        L->st->insns++;
        uint32_t pcv = addr + (thumb ? 4 : 8);
        unsigned cond = insn_cond(&i, thumb, it);
        int uncond = cond == 15, stop = 0;
        switch (i.op) {
            case OP_B: {
                uint32_t t = pcv + i.imm;
                add_entry(L, t, thumb, t <= addr);
                stop = uncond;
                break;
            }
            case OP_BL:
            case OP_BLX_IMM: {
                uint32_t base = i.op == OP_BLX_IMM && thumb ? (pcv & ~3u) : pcv, t = base + i.imm;
                add_entry(L, t, i.op == OP_BLX_IMM ? !thumb : thumb, 1);
                add_entry(L, addr + len, thumb, 1);
                stop = uncond;
                break;
            }
            case OP_BLX_REG:
                add_entry(L, addr + len, thumb, 1);
                stop = uncond;
                break;
            case OP_BX:
                if (!thumb && uncond) discover_computed_offset(L, &i, addr, rec, nrec);
                stop = uncond;
                break;
            case OP_CBZ: {
                uint32_t t = pcv + i.imm;
                add_entry(L, t, thumb, t <= addr);
                break;
            }
            case OP_TB:
                discover_tb(L, &i, pcv, rec, nrec);
                stop = uncond;
                break;
            case OP_DP:
                if (i.sub == 4 && i.k == K_RSI && i.sh == 0 && i.sa == 0) discover_pc_relative(L, &i, addr, thumb, rec, nrec);
                if (!thumb && i.sub == 13 && i.rd == 14 && i.k == K_RSI && i.rm == 15 && i.sa == 0)
                    add_entry(L, addr + 8, 0, 1); /* mov lr, pc; b/ldr pc: returns to addr + 8 */
                if (i.rd == 15 && !(i.sub >= 8 && i.sub <= 11)) {
                    stop = uncond;
                    if (!thumb && i.sub == 4 && i.rn != 15 && i.k == K_RSI && i.sh == 0 && i.sa == 0) discover_offset_table(L, &i, addr, rec, nrec);
                    if (!thumb && i.sub == 4 && i.rn == 15) { /* add pc, pc, rX, lsl #2 followed by a table of branches: every one is an entry */
                        for (uint32_t t = addr + 4;; t += 4) {
                            A32Insn bi;
                            unsigned bl = decode_at(L, t, 0, &bi);
                            if (!bl || bi.op != OP_B || bi.cond != 15) break;
                            add_entry(L, t, 0, 1);
                        }
                    }
                }
                break;
            case OP_LDST:
                if (i.rd == 15 && i.sub == 1) stop = uncond;
                break;
            case OP_LDM:
                if (i.sub && (i.imm >> 15 & 1)) stop = uncond;
                break;
            case OP_UNDEF:
            case OP_UNIMPL: stop = 1; break;
            default: break;
        }
        if (nrec == RECENT) {
            memmove(rec, rec + 1, (RECENT - 1) * sizeof *rec);
            nrec--;
        }
        rec[nrec].addr = addr;
        rec[nrec].i = i;
        nrec++;
        if (i.op == OP_IT) it = i.imm;
        else if (thumb && it) it = it_advance(it);
        if (stop) {
            /* Code that follows an unconditional transfer is usually the next function (reached by pc-relative pointers, tables or calls the walk
             * could not follow): continue there, unless it does not decode. Data decoded as code costs image size, never correctness. */
            if (looks_like_prologue(L, addr + len, thumb)) add_entry(L, addr + len, thumb, 1);
            break;
        }
        addr += len;
    }
}

/* Function openers nothing points at (pc-relative function pointers, calls through registers the walk cannot resolve): every ARM `push {..., lr}`, and every
 * Thumb push right after something that ends a function. Wrong guesses decode a little data as code; they are never executed. */
static void scan_prologues(Lift *L) {
    for (unsigned m = 0; m < L->in->nmods; m++) {
        uint32_t base = L->mf[m].base, end = base + L->mf[m].text_end;
        for (uint32_t a = base; a + 4 <= end; a += 2) {
            if (!(a & 2) && (ld32g(L, a) & 0xffff4000u) == 0xe92d4000u) add_entry(L, a, 0, 1);
            uint32_t h = ld16g(L, a);
            int push = (h & 0xff00) == 0xb500 || (h == 0xe92d && (ld16g(L, a + 2) & 0x4000));
            if (!push || a < base + 4) continue;
            uint32_t p = ld16g(L, a - 2), pp = ld16g(L, a - 4);
            int ends = p == 0x4770 || (p & 0xff00) == 0xbd00 || (p & 0xf800) == 0xe000 || p == 0xbf00 || p == 0x46c0 || p == 0 ||
                       (pp == 0xe8bd && (p & 0x8000)) || (pp == 0xf85d && p == 0xfb04); /* pop.w {.., pc}; ldr pc, [sp], #4 */
            if (ends) add_entry(L, a, 1, 1);
        }
    }
}

/* Code nothing points at that is only reached through values the walk could not follow (function pointers computed in ways no recogniser knows, tables built at
 * run time): every stretch of the text that no walk visited is walked too, in the mode (ARM or Thumb) of the code before it. A literal pool or table in such a stretch
 * decodes as garbage instructions that are lifted and never executed (image size, not correctness); the benefit is that the image has a block for every address a
 * real function can start at, so a computed branch never leaves the lifted code. */
static void sweep_gaps(Lift *L) {
    for (unsigned m = 0; m < L->in->nmods; m++) {
        ModFlags *f = &L->mf[m];
        int thumb = 0;
        for (uint32_t off = 0; off < f->text_end; off += 2) {
            uint8_t va = (off & 2) ? 0 : f->fa[off >> 2] & F_VIS, vt = f->ft[off >> 1] & F_VIS;
            if (va) thumb = 0;
            else if (vt) thumb = 1;
            if (va || vt || (!thumb && (off & 2))) continue;
            add_entry(L, f->base + off, thumb, 1);
            while (L->nstack) {
                uint32_t e = L->stack[--L->nstack];
                walk_run(L, e & ~1u, e & 1);
            }
        }
    }
}

static void discover(Lift *L) {
    for (unsigned s = 0; s < L->in->nseeds; s++) add_entry(L, L->in->seeds[s] & ~1u, L->in->seeds[s] & 1, 1);
    scan_prologues(L);
    while (L->nstack) {
        uint32_t e = L->stack[--L->nstack];
        walk_run(L, e & ~1u, e & 1);
    }
    if (!getenv("A32LIFT_NOSWEEP")) sweep_gaps(L);
}

/* ---- emission helpers ---------------------------------------------------------------------------------------------- */
static void movc(Lift *L, const char *w, uint32_t v) {
    if (v <= 0xffff) out(L, "\tmovz %s, #0x%x", w, v);
    else if (~v <= 0xffff) out(L, "\tmovn %s, #0x%x", w, ~v & 0xffff);
    else if (!(v & 0xffff)) out(L, "\tmovz %s, #0x%x, lsl #16", w, v >> 16);
    else {
        out(L, "\tmovz %s, #0x%x", w, v & 0xffff);
        out(L, "\tmovk %s, #0x%x, lsl #16", w, v >> 16);
    }
}
static uint32_t chunk_of(uint32_t a) { return a >> CHUNK_SHIFT; }
static void lname(char *b, size_t n, uint32_t a, int thumb, int local) { snprintf(b, n, "%s%c_%08x", local ? "L" : "", thumb ? 'T' : 'A', a); }

/* Jump to a guest address known at lift time. `cond` 0..13 = only when that A64 condition holds. */
static void jump_to(Lift *L, uint32_t t, int tthumb, int cond) {
    uint8_t *f = flagref(L, t, tthumb);
    char nm[32];
    if (!f || !(*f & F_ENT)) { /* nothing lifted there: leave to the dispatcher (which interprets) */
        unsigned lab = L->label++;
        if (cond < 14) out(L, "\tb.%s L%u", CC[cond ^ 1], lab);
        movc(L, "w24", t | tthumb);
        out(L, "\tb _a32jit_noblock");
        if (cond < 14) out(L, "L%u:", lab);
        return;
    }
    int same = L->cur_chunk == chunk_of(t);
    lname(nm, sizeof nm, t, tthumb, same);
    if (cond >= 14) {
        out(L, "\tb %s", nm);
        return;
    }
    if (same) {
        out(L, "\tb.%s %s", CC[cond], nm);
        return;
    }
    unsigned lab = L->label++;
    out(L, "\tb.%s L%u", CC[cond ^ 1], lab);
    out(L, "\tb %s", nm);
    out(L, "L%u:", lab);
}

/* Indirect branch to the guest address in `src` (a w register or "w24"). kind 0 = BX (bit 0 selects Thumb), 1 = Thumb ALU write (stay Thumb). */
static void jump_ind(Lift *L, const char *src, int kind) {
    if (kind) out(L, "\torr w24, %s, #1", src);
    else {
        unsigned lab = L->label++;
        if (strcmp(src, "w24")) out(L, "\tmov w24, %s", src);
        out(L, "\ttbnz w24, #0, L%u", lab);
        out(L, "\tand w24, w24, #0xfffffffc");
        out(L, "L%u:", lab);
    }
    out(L, "\tb _a32jit_ind");
}

/* ---- operand 2 ------------------------------------------------------------------------------------------------------ */
typedef struct {
    char text[64];
    int is_imm;
    uint32_t imm;
    int carry; /* 0 unchanged, 1 constant bit in cbit, 2 bit `cbit` of register `csrc` (an x register name) */
    int cbit;
    char csrc[8];
} Op2;

/* Builds operand 2 of a data-processing instruction. logical: the A64 form accepts a ror shifted operand; plain: must be a register (no shift
 * left in the instruction text). The register-shifted forms and every materialisation go through w17 (and w26..w28). Returns 0 if not supported. */
static int build_op2(Lift *L, const A32Insn *i, uint32_t pcv, int logical, int plain, Op2 *o) {
    memset(o, 0, sizeof *o);
    if (i->k == K_IMM) {
        o->is_imm = 1;
        o->imm = i->imm;
        if (i->fl & FL_ICARRY) {
            o->carry = 1;
            o->cbit = i->imm >> 31;
        }
        if (plain) {
            movc(L, "w16", i->imm);
            strcpy(o->text, "w16");
        }
        return 1;
    }
    const char *src;
    char xs[8];
    if (i->rm == 15) {
        movc(L, "w26", pcv);
        src = "w26";
        strcpy(xs, "x26");
    } else {
        src = W[i->rm];
        snprintf(xs, sizeof xs, "x%u", i->rm);
    }
    if (i->k == K_RSR) {
        const char *ws = W[i->ra];
        if (i->ra == 15) return 0;
        switch (i->sh) {
            case 0:
            case 1:
                out(L, "\tand w17, %s, #0xff", ws);
                out(L, "\t%s x28, %s, x17", i->sh ? "lsr" : "lsl", xs);
                out(L, "\tlsr w17, w17, #6");
                out(L, "\tsub w17, w17, #1");
                out(L, "\tasr w17, w17, #31");
                out(L, "\tand w17, w28, w17");
                break;
            case 2:
                out(L, "\tand w17, %s, #0xff", ws);
                out(L, "\tsxtw x28, %s", src);
                out(L, "\tlsr w27, w17, #6");
                out(L, "\tneg w27, w27");
                out(L, "\tasr w27, w27, #31");
                out(L, "\torr w17, w17, w27");
                out(L, "\tasr x28, x28, x17");
                out(L, "\tmov w17, w28");
                break;
            default: out(L, "\tror w17, %s, %s", src, ws); break;
        }
        strcpy(o->text, "w17");
        o->carry = -1; /* the shifter carry is not modelled: callers that need it fall back */
        return 1;
    }
    unsigned sh = i->sh, sa = i->sa;
    o->carry = 2;
    snprintf(o->csrc, sizeof o->csrc, "%s", xs);
    switch (sh) {
        case 0:
            if (sa == 0) o->carry = 0;
            else o->cbit = 32 - sa;
            break;
        case 1: o->cbit = sa == 0 ? 31 : sa - 1; break;
        case 2: o->cbit = sa == 0 ? 31 : sa - 1; break;
        default: o->cbit = sa == 0 ? 0 : sa - 1; break;
    }
    if (sh == 3 && sa == 0) { /* rrx */
        out(L, "\tcset w17, hs");
        out(L, "\textr w17, w17, %s, #1", src);
        strcpy(o->text, "w17");
        return 1;
    }
    if (!plain && (sh != 3 || logical) && !(sh == 1 && sa == 0)) {
        static const char *const sn[4] = {"lsl", "lsr", "asr", "ror"};
        unsigned a = (sh == 2 && sa == 0) ? 31 : sa;
        if (sa == 0 && sh == 0) strcpy(o->text, src);
        else snprintf(o->text, sizeof o->text, "%s, %s #%u", src, sn[sh], a);
        return 1;
    }
    if (sh == 1 && sa == 0) {
        if (plain) {
            out(L, "\tmovz w17, #0");
            strcpy(o->text, "w17");
        } else strcpy(o->text, "wzr");
        return 1;
    }
    if (sh == 0 && sa == 0) {
        strcpy(o->text, src);
        return 1;
    }
    {
        static const char *const sn[4] = {"lsl", "lsr", "asr", "ror"};
        unsigned a = (sh == 2 && sa == 0) ? 31 : sa;
        out(L, "\t%s w17, %s, #%u", sn[sh], src, a);
        strcpy(o->text, "w17");
    }
    return 1;
}

/* ---- instruction families --------------------------------------------------------------------------------------------- */
enum { LIFT_NO = 0, LIFT_OK = 1, LIFT_TERM = 2 };

/* Operand 2 of a flag-setting logical instruction with a register-controlled shift (`tst r0, r1, lsl r2`): the shifter carry depends on the run-time amount. Leaves the
 * shifted value in w17 and the carry out in bit 0 of x24 (the old C when the amount is 0). x28 must hold the saved NZCV. */
static void emit_rsr_shift_carry(Lift *L, const A32Insn *i) {
    const char *wm = W[i->rm], *ws = W[i->ra];
    unsigned big = L->label++, zero = L->label++, done = L->label++;
    out(L, "\tand w26, %s, #0xff", ws);
    out(L, "\tcbz w26, L%u", zero);
    switch (i->sh) {
        case 0: /* lsl */
            out(L, "\tcmp w26, #32\n\tb.hs L%u", big);
            out(L, "\tlslv w17, %s, w26\n\tmovz w24, #32\n\tsub w24, w24, w26\n\tlsrv w24, %s, w24", wm, wm);
            out(L, "\tb L%u", done);
            out(L, "L%u:\n\tmovz w17, #0\n\tand w24, %s, #1\n\tcmp w26, #32\n\tcsel w24, w24, wzr, eq\n\tb L%u", big, wm, done);
            break;
        case 1: /* lsr */
            out(L, "\tcmp w26, #32\n\tb.hs L%u", big);
            out(L, "\tlsrv w17, %s, w26\n\tsub w24, w26, #1\n\tlsrv w24, %s, w24", wm, wm);
            out(L, "\tb L%u", done);
            out(L, "L%u:\n\tmovz w17, #0\n\tlsr w24, %s, #31\n\tcmp w26, #32\n\tcsel w24, w24, wzr, eq\n\tb L%u", big, wm, done);
            break;
        case 2: /* asr */
            out(L, "\tcmp w26, #32\n\tb.hs L%u", big);
            out(L, "\tasrv w17, %s, w26\n\tsub w24, w26, #1\n\tlsrv w24, %s, w24", wm, wm);
            out(L, "\tb L%u", done);
            out(L, "L%u:\n\tasr w17, %s, #31\n\tlsr w24, %s, #31\n\tb L%u", big, wm, wm, done);
            break;
        default: /* ror */
            out(L, "\tands w24, w26, #31\n\tb.eq L%u", big);
            out(L, "\trorv w17, %s, w26\n\tlsr w24, w17, #31\n\tb L%u", wm, done);
            out(L, "L%u:\n\tmov w17, %s\n\tlsr w24, %s, #31\n\tb L%u", big, wm, wm, done);
            break;
    }
    out(L, "L%u:\n\tmov w17, %s\n\tubfx x24, x28, #29, #1", zero, wm);
    out(L, "L%u:", done);
}

static int lift_dp(Lift *L, const A32Insn *i, int setf) {
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8);
    unsigned sub = i->sub, rd = i->rd;
    int wr = !(sub >= 8 && sub <= 11);
    if (sub > 16) return LIFT_NO;
    if (!wr) setf = 1;
    if (rd == 15 && wr && setf) return LIFT_NO;
    int logical = sub == 0 || sub == 1 || sub == 8 || sub == 9 || sub == 12 || sub == 13 || sub == 14 || sub == 15 || sub == 16;
    int rsr_flags = i->k == K_RSR && logical && setf;
    if (rsr_flags && (i->rm == 15 || i->ra == 15)) return LIFT_NO;
    const char *a = "wzr";
    if (sub != 13 && sub != 15) {
        if (i->rn == 15) {
            movc(L, "w15", i->k == K_IMM ? (pcv & ~3u) : pcv);
            a = "w15";
        } else a = W[i->rn];
    }
    const char *d = !wr ? "wzr" : rd == 15 ? "w24" : W[rd];
    int needs_plain = !logical && (sub == 3 || sub == 5 || sub == 6 || sub == 7);
    Op2 o;
    if (logical) {
        int imm_mov = i->k == K_IMM && (sub == 13 || sub == 15);
        if (rsr_flags) {
            memset(&o, 0, sizeof o);
            out(L, "\tmrs x28, nzcv");
            emit_rsr_shift_carry(L, i);
            strcpy(o.text, "w17");
            o.carry = 3;
        } else if (!imm_mov) {
            if (!build_op2(L, i, pcv, 1, i->k == K_IMM, &o)) return LIFT_NO;
        } else {
            memset(&o, 0, sizeof o);
            o.is_imm = 1;
            o.imm = i->imm;
            if (i->fl & FL_ICARRY) {
                o.carry = 1;
                o.cbit = i->imm >> 31;
            }
        }
        char csrc[8] = "";
        if (setf) {
            if (!rsr_flags) out(L, "\tmrs x28, nzcv");
            if (o.carry == 2) {
                snprintf(csrc, sizeof csrc, "%s", o.csrc);
                if (!strcmp(d, W[i->rm]) && i->k == K_RSI && i->rm < 15) {
                    out(L, "\tmov w27, %s", W[i->rm]);
                    strcpy(csrc, "x27");
                }
            }
        }
        const char *dd = d, *B = o.text;
        int need_tst = setf;
        switch (sub) {
            case 0:
                out(L, "\t%s %s, %s, %s", setf ? "ands" : "and", d, a, B);
                need_tst = 0;
                break;
            case 8:
                out(L, "\tands wzr, %s, %s", a, B);
                need_tst = 0;
                break;
            case 1: out(L, "\teor %s, %s, %s", d, a, B); break;
            case 9:
                out(L, "\teor w27, %s, %s", a, B);
                dd = "w27";
                break;
            case 12: out(L, "\torr %s, %s, %s", d, a, B); break;
            case 14:
                out(L, "\t%s %s, %s, %s", setf ? "bics" : "bic", d, a, B);
                need_tst = 0;
                break;
            case 16: out(L, "\torn %s, %s, %s", d, a, B); break;
            case 13:
                if (i->k == K_IMM) movc(L, d, i->imm);
                else out(L, "\torr %s, wzr, %s", d, B);
                break;
            default:
                if (i->k == K_IMM) movc(L, d, ~i->imm);
                else out(L, "\tmvn %s, %s", d, B);
                break; /* 15 */
        }
        if (setf) {
            if (need_tst) out(L, "\ttst %s, %s", dd, dd);
            out(L, "\trmif x28, #28, #%d", o.carry == 0 ? 3 : 1);
            if (o.carry == 1) {
                out(L, "\tmovz x27, #%d", o.cbit);
                out(L, "\trmif x27, #63, #2");
            } else if (o.carry == 2) out(L, "\trmif %s, #%u, #2", csrc, (unsigned)((o.cbit - 1) & 63));
            else if (o.carry == 3) out(L, "\trmif x24, #63, #2"); /* the run-time shifter carry */
        }
    } else {
        int imm12 = i->k == K_IMM && i->imm <= 0xfff && (sub == 2 || sub == 4 || sub == 10 || sub == 11);
        if (imm12) {
            memset(&o, 0, sizeof o);
            snprintf(o.text, sizeof o.text, "#%u", i->imm);
        } else if (!build_op2(L, i, pcv, 0, needs_plain || i->k == K_IMM, &o)) return LIFT_NO;
        const char *S = setf ? "s" : "";
        switch (sub) {
            case 2: out(L, "\tsub%s %s, %s, %s", S, d, a, o.text); break;
            case 3: out(L, "\tsub%s %s, %s, %s", S, d, o.text, a); break;
            case 4: out(L, "\tadd%s %s, %s, %s", S, d, a, o.text); break;
            case 5: out(L, "\tadc%s %s, %s, %s", S, d, a, o.text); break;
            case 6: out(L, "\tsbc%s %s, %s, %s", S, d, a, o.text); break;
            case 7: out(L, "\tsbc%s %s, %s, %s", S, d, o.text, a); break;
            case 10: out(L, "\tcmp %s, %s", a, o.text); break;
            default: out(L, "\tcmn %s, %s", a, o.text); break; /* 11 */
        }
    }
    if (rd == 15 && wr) {
        jump_ind(L, "w24", L->thumb ? 1 : 0);
        return LIFT_TERM;
    }
    return LIFT_OK;
}

static int lift_mul(Lift *L, const A32Insn *i, int setf) {
    if (setf || i->rd == 15 || i->rn == 15 || i->rm == 15) return LIFT_NO;
    if (i->op == OP_MUL) {
        if (i->sub > 0 && i->ra == 15) return LIFT_NO;
        if (i->sub == 0) out(L, "\tmul %s, %s, %s", W[i->rd], W[i->rn], W[i->rm]);
        else if (i->sub == 1) out(L, "\tmadd %s, %s, %s, %s", W[i->rd], W[i->rn], W[i->rm], W[i->ra]);
        else out(L, "\tmsub %s, %s, %s, %s", W[i->rd], W[i->rn], W[i->rm], W[i->ra]);
        return LIFT_OK;
    }
    /* OP_MULL: umull umlal smull smlal (umaal falls back) */
    if (i->sub > 3 || i->ra == 15) return LIFT_NO;
    const char *u = (i->sub < 2) ? "umaddl" : "smaddl", *ml = (i->sub < 2) ? "umull" : "smull";
    if (i->sub & 1) {
        out(L, "\torr x16, %s, %s, lsl #32", X[i->rd], X[i->ra]);
        out(L, "\t%s x16, %s, %s, x16", u, W[i->rn], W[i->rm]);
    } else out(L, "\t%s x16, %s, %s", ml, W[i->rn], W[i->rm]);
    out(L, "\tlsr x17, x16, #32");
    out(L, "\tmov %s, w16", W[i->rd]);
    out(L, "\tmov %s, w17", W[i->ra]);
    return LIFT_OK;
}

static int lift_misc(Lift *L, const A32Insn *i) {
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8);
    (void)pcv;
    if (i->rd == 15) return LIFT_NO;
    switch (i->op) {
        case OP_DIV:
            if (i->rn == 15 || i->rm == 15) return LIFT_NO;
            out(L, "\t%s %s, %s, %s", i->sub ? "udiv" : "sdiv", W[i->rd], W[i->rn], W[i->rm]);
            return LIFT_OK;
        case OP_CLZ:
            if (i->rm == 15) return LIFT_NO;
            out(L, "\tclz %s, %s", W[i->rd], W[i->rm]);
            return LIFT_OK;
        case OP_RBIT:
            if (i->rm == 15) return LIFT_NO;
            out(L, "\trbit %s, %s", W[i->rd], W[i->rm]);
            return LIFT_OK;
        case OP_REV:
            if (i->rm == 15) return LIFT_NO;
            out(L, "\trev %s, %s", W[i->rd], W[i->rm]);
            return LIFT_OK;
        case OP_REV16:
            if (i->rm == 15) return LIFT_NO;
            out(L, "\trev16 %s, %s", W[i->rd], W[i->rm]);
            return LIFT_OK;
        case OP_REVSH:
            if (i->rm == 15) return LIFT_NO;
            out(L, "\trev16 w16, %s", W[i->rm]);
            out(L, "\tsxth %s, w16", W[i->rd]);
            return LIFT_OK;
        case OP_EXT: {
            if (i->sub > 3 || i->rm == 15) return LIFT_NO;
            static const char *const ext[4] = {"sxtb", "sxth", "uxtb", "uxth"};
            const char *src = W[i->rm];
            if (i->sa) {
                out(L, "\tror w16, %s, #%u", src, i->sa);
                src = "w16";
            }
            if (i->rn == 15) out(L, "\t%s %s, %s", ext[i->sub], W[i->rd], src);
            else out(L, "\tadd %s, %s, %s, %s", W[i->rd], W[i->rn], src, ext[i->sub]);
            return LIFT_OK;
        }
        case OP_BFX:
            if (i->rn == 15 || i->imm2 == 0 || i->imm + i->imm2 > 32) return LIFT_NO;
            out(L, "\t%s %s, %s, #%u, #%u", i->sub ? "sbfx" : "ubfx", W[i->rd], W[i->rn], i->imm, i->imm2);
            return LIFT_OK;
        case OP_BFI:
            if (i->imm2 == 0 || i->imm + i->imm2 > 32) return LIFT_NO;
            out(L, "\tbfi %s, %s, #%u, #%u", W[i->rd], i->rn == 15 ? "wzr" : W[i->rn], i->imm, i->imm2);
            return LIFT_OK;
        default: return LIFT_NO;
    }
}

static int lift_smul(Lift *L, const A32Insn *i) {
    if (i->rd == 15 || i->rn == 15 || i->rm == 15) return LIFT_NO;
    if (i->op == OP_SMULH && i->sub == 3) { /* smul<x><y> */
        out(L, "\tsbfx w16, %s, #%u, #16", W[i->rn], (i->imm & 1) ? 16 : 0);
        out(L, "\tsbfx w17, %s, #%u, #16", W[i->rm], (i->imm & 2) ? 16 : 0);
        out(L, "\tmul %s, w16, w17", W[i->rd]);
        return LIFT_OK;
    }
    if (i->op == OP_SMMUL && i->sub == 0 && i->ra == 15 && !i->imm) {
        out(L, "\tsmull x16, %s, %s", W[i->rn], W[i->rm]);
        out(L, "\tlsr x16, x16, #32");
        out(L, "\tmov %s, w16", W[i->rd]);
        return LIFT_OK;
    }
    return LIFT_NO;
}

static int lift_ldst_plain(Lift *L, const A32Insn *i) {
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8);
    unsigned sub = i->sub, rn = i->rn, rd = i->rd;
    int wb = (i->fl & FL_W) != 0, pre = (i->fl & FL_P) != 0, up = (i->fl & FL_U) != 0;
    int dbl = sub >= 8, load = dbl ? sub == 9 : (sub & 1 || sub >= 6);
    if (rn == 15 && wb) return LIFT_NO;
    if (dbl && (rd == 15 || i->ra == 15 || rd == i->ra)) return LIFT_NO;
    if (!load && !dbl && rd == 15) return LIFT_NO;
    if (load && !dbl && rd == 15 && sub != 1) return LIFT_NO;
    if ((i->fl & FL_REG) && i->rm == 15) return LIFT_NO;
    const char *base;
    if (rn == 15) {
        movc(L, "w15", pcv & ~3u);
        base = "w15";
    } else base = W[rn];
    /* effective addresses: w16 = base +- offset */
    int need_off = (i->fl & FL_REG) ? 1 : (i->imm != 0);
    const char *off_addr = base;
    if (need_off || wb) {
        if (i->fl & FL_REG) {
            A32Insn t = *i;
            t.k = K_RSI;
            Op2 o;
            if (!build_op2(L, &t, pcv, 0, 0, &o)) return LIFT_NO;
            out(L, "\t%s w16, %s, %s", up ? "add" : "sub", base, o.text);
        } else if (i->imm <= 0xfff) out(L, "\t%s w16, %s, #%u", up ? "add" : "sub", base, i->imm);
        else {
            movc(L, "w17", i->imm);
            out(L, "\t%s w16, %s, w17", up ? "add" : "sub", base);
        }
        off_addr = "w16";
    }
    const char *addr = pre ? off_addr : base;
    char amode[48];
    if (load && (wb || dbl)) {
        out(L, "\tadd x17, x20, %s, uxtw", addr);
        strcpy(amode, "[x17]");
    } else snprintf(amode, sizeof amode, "[x20, %s, uxtw]", addr);
    if (dbl) {
        if (load) {
            if (wb) out(L, "\tmov %s, w16", W[rn]);
            out(L, "\tldp %s, %s, [x17]", W[rd], W[i->ra]);
        } else {
            out(L, "\tadd x17, x20, %s, uxtw", addr);
            out(L, "\tstp %s, %s, [x17]", W[rd], W[i->ra]);
            if (wb) out(L, "\tmov %s, w16", W[rn]);
        }
        return LIFT_OK;
    }
    if (!load) {
        static const char *const st[5] = {"str", "", "strb", "", "strh"};
        out(L, "\t%s %s, %s", st[sub], W[rd], amode);
        if (wb) out(L, "\tmov %s, w16", W[rn]);
        return LIFT_OK;
    }
    static const char *const ld[8] = {"", "ldr", "", "ldrb", "", "ldrh", "ldrsb", "ldrsh"};
    if (wb && rn != 15) out(L, "\tmov %s, w16", W[rn]);
    if (rd == 15) {
        out(L, "\tldr w24, %s", amode);
        jump_ind(L, "w24", 0);
        return LIFT_TERM;
    }
    out(L, "\t%s %s, %s", ld[sub], W[rd], amode);
    return LIFT_OK;
}

/* lda / ldab / ldah and stl / stlb / stlh (FL_ACQ): the access plus a barrier on the weakly ordered host (acquire: dmb ishld after a load; release: dmb ish before a store) */
static int lift_ldst(Lift *L, const A32Insn *i) {
    if (!(i->fl & FL_ACQ)) return lift_ldst_plain(L, i);
    A32Insn t = *i;
    t.fl &= (uint8_t)~FL_ACQ;
    int load = t.sub >= 8 ? t.sub == 9 : (t.sub & 1 || t.sub >= 6);
    if (!load) out(L, "\tdmb ish");
    int r = lift_ldst_plain(L, &t);
    if (r != LIFT_NO && load) out(L, "\tdmb ishld");
    return r;
}

static int lift_ldm(Lift *L, const A32Insn *i) {
    unsigned list = i->imm & 0xffff, rn = i->rn, n = (unsigned)__builtin_popcount(list);
    int load = i->sub != 0, wb = (i->fl & FL_W) != 0, pre = (i->fl & FL_P) != 0, up = (i->fl & FL_U) != 0;
    if (rn == 15 || n == 0) return LIFT_NO;
    if (!load && (list >> 15 & 1)) return LIFT_NO;
    int off; /* byte offset of the first transferred word from the base */
    if (up) off = pre ? 4 : 0;
    else off = pre ? -4 * (int)n : -4 * (int)n + 4;
    const char *a = W[rn];
    if (off > 0) {
        out(L, "\tadd w16, %s, #%d", a, off);
        a = "w16";
    } else if (off < 0) {
        out(L, "\tsub w16, %s, #%d", a, -off);
        a = "w16";
    }
    out(L, "\tadd x17, x20, %s, uxtw", a);
    unsigned pos = 0;
    for (unsigned r = 0; r < 16; r++) {
        if (!(list >> r & 1)) continue;
        if (r == 15) {
            out(L, "\tldr w24, [x17, #%u]", pos * 4);
            pos++;
            continue;
        }
        if (r < 14 && (list >> (r + 1) & 1)) {
            out(L, "\t%s %s, %s, [x17, #%u]", load ? "ldp" : "stp", W[r], W[r + 1], pos * 4);
            pos += 2;
            r++;
        } else {
            out(L, "\t%s %s, [x17, #%u]", load ? "ldr" : "str", W[r], pos * 4);
            pos++;
        }
    }
    if (wb && !(load && (list >> rn & 1))) out(L, "\t%s %s, %s, #%u", up ? "add" : "sub", W[rn], W[rn], 4 * n);
    if (load && (list >> 15 & 1)) {
        jump_ind(L, "w24", 0);
        return LIFT_TERM;
    }
    return LIFT_OK;
}

/* ldrex / strex (and the acquire / release forms) on the host's own exclusive monitor: the guest's retry loops see the same 0 / 1 status. The load and the
 * store of a pair are always lifted together (a lifted strex whose ldrex ran in the interpreter would never succeed). */
static int lift_excl(Lift *L, const A32Insn *i) {
    unsigned sz = i->sub, acq = (i->fl & FL_ACQ) != 0;
    if (sz > 3) return LIFT_NO;
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8);
    (void)pcv;
    if (i->rn == 15) return LIFT_NO;
    if (i->op == OP_LDREX) {
        if (i->rd == 15 || (sz == 3 && (i->ra == 15 || i->ra == i->rd))) return LIFT_NO;
    } else {
        if (i->rd == 15 || i->rm == 15 || i->rd == i->rm || i->rd == i->rn || (sz == 3 && (i->ra == 15 || i->rd == i->ra))) return LIFT_NO;
    }
    const char *base = W[i->rn];
    if (i->imm) {
        out(L, "\tadd w16, %s, #%u", base, i->imm);
        base = "w16";
    }
    out(L, "\tadd x17, x20, %s, uxtw", base);
    static const char *const sfx[3] = {"b", "h", ""};
    if (i->op == OP_LDREX) {
        if (sz == 3) out(L, "\t%s %s, %s, [x17]", acq ? "ldaxp" : "ldxp", W[i->rd], W[i->ra]);
        else out(L, "\t%s%s %s, [x17]", acq ? "ldaxr" : "ldxr", sfx[sz], W[i->rd]);
    } else {
        if (sz == 3) out(L, "\t%s %s, %s, %s, [x17]", acq ? "stlxp" : "stxp", W[i->rd], W[i->rm], W[i->ra]);
        else out(L, "\t%s%s %s, %s, [x17]", acq ? "stlxr" : "stxr", sfx[sz], W[i->rd], W[i->rm]);
    }
    return LIFT_OK;
}

static int lift_branch(Lift *L, const A32Insn *i, unsigned cond, unsigned len, int *handled_cond) {
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8), ret = (L->pc + len) | (L->thumb ? 1u : 0u);
    int a64c = cond == 15 ? 14 : (int)cond;
    switch (i->op) {
        case OP_B:
            jump_to(L, pcv + i->imm, L->thumb, a64c);
            *handled_cond = 1;
            return cond == 15 ? LIFT_TERM : LIFT_OK;
        case OP_CBZ: {
            uint32_t t = pcv + i->imm;
            uint8_t *f = flagref(L, t, 1);
            if (f && (*f & F_ENT) && chunk_of(t) == L->cur_chunk) {
                char nm[32];
                lname(nm, sizeof nm, t, 1, 1);
                out(L, "\t%s %s, %s", i->sub ? "cbnz" : "cbz", W[i->rn], nm);
            } else {
                unsigned lab = L->label++;
                out(L, "\t%s %s, L%u", i->sub ? "cbz" : "cbnz", W[i->rn], lab);
                jump_to(L, t, 1, 14);
                out(L, "L%u:", lab);
            }
            *handled_cond = 1;
            return LIFT_OK;
        }
        default: break;
    }
    (void)ret;
    return LIFT_NO;
}


/* A32LIFT_INLINE=<mask>: which groups of VFP/NEON instructions are lifted inline (bisecting aid; default all): 1 NEON load/store, 2 NEON integer and bitwise, 4 NEON float,
 * 8 VFP load/store and moves, 16 VFP arithmetic, 32 VFP convert/compare */
static unsigned inline_mask(void) {
    static int init;
    static unsigned m = 63;
    if (!init) {
        init = 1;
        if (getenv("A32LIFT_INLINE")) m = (unsigned)strtoul(getenv("A32LIFT_INLINE"), NULL, 0);
    }
    return m;
}
/* ---- VFP / NEON, the parts that need no floating point semantics ------------------------------------------------------------------
 * The register file lives in the A32Cpu at x21 + offsetof(v_); x15 is pointed at it for the instruction. Loads, stores, register moves, the bitwise and the
 * integer NEON operations are lifted to host SIMD instructions; arithmetic that depends on FPSCR (rounding, flush to zero, NaN rules, exception flags) is
 * left to the out-of-line operations. `w` is the A32-form instruction word the decoder keeps in A32Insn.imm. */
#define VOFF offsetof(A32Cpu, v_)
static void vbase(Lift *L) { out(L, "\tadd x15, x21, #%zu", VOFF); }
static unsigned vreg_d(uint32_t w) { return (w >> 22 & 1) << 4 | (w >> 12 & 15); }
static unsigned vreg_n(uint32_t w) { return (w >> 7 & 1) << 4 | (w >> 16 & 15); }
static unsigned vreg_m(uint32_t w) { return (w >> 5 & 1) << 4 | (w & 15); }

static void ldq(Lift *L, unsigned v, unsigned dreg, int q) { out(L, "\tldr %c%u, [x15, #%u]", q ? 'q' : 'd', v, 8 * dreg); }
static void stq(Lift *L, unsigned v, unsigned dreg, int q) { out(L, "\tstr %c%u, [x15, #%u]", q ? 'q' : 'd', v, 8 * dreg); }

/* VLD1 / VST1 single element to one lane, and VLD1 to all lanes (vld1.N {Dd[]} / {Dd[], Dd+1[]}) */
static int lift_neon_ls_lane(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned size = w >> 10 & 3, n = w >> 8 & 3, rn = w >> 16 & 15, rm = w & 15, d = vreg_d(w), load = w >> 21 & 1;
    if (n != 0 || rn == 15) return LIFT_NO; /* vld2..4 stay out of line */
    vbase(L);
    out(L, "\tadd x17, x20, %s, uxtw", W[rn]);
    unsigned ebytes;
    if (size == 3) { /* all lanes */
        if (!load) return LIFT_NO;
        unsigned t = w >> 5 & 1, esz = w >> 6 & 3;
        if (esz == 3) return LIFT_NO;
        ebytes = 1u << esz;
        static const char *const ar[3] = {"16b", "8h", "4s"};
        out(L, "\tld1r {v0.%s}, [x17]", ar[esz]);
        out(L, "\tstr d0, [x15, #%u]", 8 * d);
        if (t) {
            if (d + 1 > 31) return LIFT_NO;
            out(L, "\tstr d0, [x15, #%u]", 8 * (d + 1));
        }
    } else {
        unsigned ia = w >> 4 & 15, idx;
        static const char *const sfx[3] = {"b", "h", "s"};
        if (size == 0) idx = ia >> 1 & 7;
        else if (size == 1) idx = ia >> 2 & 3;
        else idx = ia >> 3 & 1;
        ebytes = 1u << size;
        out(L, "\tldr d0, [x15, #%u]", 8 * d);
        out(L, "\t%s {v0.%s}[%u], [x17]", load ? "ld1" : "st1", sfx[size], idx);
        if (load) out(L, "\tstr d0, [x15, #%u]", 8 * d);
    }
    if (rm == 13) out(L, "\tadd %s, %s, #%u", W[rn], W[rn], ebytes * (size == 3 ? ((w >> 5 & 1) + 1) * 0 + 1 : 1));
    else if (rm != 15) out(L, "\tadd %s, %s, %s", W[rn], W[rn], W[rm]);
    return LIFT_OK;
}

/* VLD1 / VST1, multiple structures, 1 to 4 registers (the element size does not matter: little-endian bytes) */
static int lift_neon_ls(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    if ((w & 0xff900000u) != 0xf4000000u)
        return (w & 0xff900000u) == 0xf4800000u ? lift_neon_ls_lane(L, i) : LIFT_NO; /* bit 23 set = single lane / all lanes */
    unsigned type = w >> 8 & 15, rn = w >> 16 & 15, rm = w & 15, d = vreg_d(w), load = w >> 21 & 1, regs;
    switch (type) {
        case 7: regs = 1; break;
        case 10: regs = 2; break;
        case 6: regs = 3; break;
        case 2: regs = 4; break;
        default: return LIFT_NO;
    }
    if (rn == 15 || d + regs > 32) return LIFT_NO;
    vbase(L);
    out(L, "\tadd x17, x20, %s, uxtw", W[rn]);
    for (unsigned k = 0; k < regs;) {
        if (regs - k >= 2 && !((d + k) & 1)) {
            if (load) out(L, "\tldr q0, [x17, #%u]\n\tstr q0, [x15, #%u]", 8 * k, 8 * (d + k));
            else out(L, "\tldr q0, [x15, #%u]\n\tstr q0, [x17, #%u]", 8 * (d + k), 8 * k);
            k += 2;
        } else {
            if (load) out(L, "\tldr d0, [x17, #%u]\n\tstr d0, [x15, #%u]", 8 * k, 8 * (d + k));
            else out(L, "\tldr d0, [x15, #%u]\n\tstr d0, [x17, #%u]", 8 * (d + k), 8 * k);
            k++;
        }
    }
    if (rm == 13) out(L, "\tadd %s, %s, #%u", W[rn], W[rn], 8 * regs);
    else if (rm != 15) out(L, "\tadd %s, %s, %s", W[rn], W[rn], W[rm]);
    return LIFT_OK;
}

/* NEON three registers of the same length: bitwise operations and the plain integer ones */
static int lift_neon_3same(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned U = w >> 24 & 1, size = w >> 20 & 3, opc = w >> 8 & 15, c4 = w >> 4 & 1, q = w >> 6 & 1, d = vreg_d(w), n = vreg_n(w), m = vreg_m(w);
    if (q && ((d | n | m) & 1)) return LIFT_NO;
    const char *arr[2][4] = {{"8b", "4h", "2s", ""}, {"16b", "8h", "4s", "2d"}};
    const char *a = arr[q][size], *op = NULL;
    int bitwise = opc == 1 && c4, three = 0, scalar = 0;
    if (bitwise) a = q ? "16b" : "8b";
    else if (!*a && !q && size == 3 && opc == 8 && !c4) scalar = 1; /* vadd.i64 / vsub.i64 on a D register: the scalar form */
    else if (!*a) return LIFT_NO;
    if (bitwise) {
        static const char *const lo[4] = {"and", "bic", "orr", "orn"}, *const hi[4] = {"eor", "bsl", "bit", "bif"};
        op = U ? hi[size] : lo[size];
        three = U && size; /* bsl / bit / bif read the destination */
    } else if (opc == 8 && !c4) op = U ? "sub" : "add";
    else if (opc == 8 && c4) {
        if (size == 3) return LIFT_NO;
        op = U ? "cmeq" : "cmtst";
    } else if (opc == 9 && c4 && !U) {
        if (size == 3) return LIFT_NO;
        op = "mul";
    } else if (opc == 3) {
        if (size == 3) return LIFT_NO;
        op = c4 ? (U ? "cmhs" : "cmge") : (U ? "cmhi" : "cmgt");
    } else if (opc == 6) {
        if (size == 3) return LIFT_NO;
        op = c4 ? (U ? "umin" : "smin") : (U ? "umax" : "smax");
    } else return LIFT_NO;
    vbase(L);
    ldq(L, 1, n, q);
    ldq(L, 2, m, q);
    if (scalar) {
        out(L, "\t%s d0, d1, d2", op);
        stq(L, 0, d, q);
        return LIFT_OK;
    }
    if (three) ldq(L, 0, d, q);
    if (!strcmp(op, "orn") || !strcmp(op, "bic") || !strcmp(op, "and") || !strcmp(op, "orr") || !strcmp(op, "eor") || three)
        out(L, "\t%s v0.%s, v1.%s, v2.%s", op, a, a, a);
    else if (!strcmp(op, "orn") && 0) return LIFT_NO;
    else out(L, "\t%s v0.%s, v1.%s, v2.%s", op, a, a, a);
    if (size == 3 && !q && !bitwise) return LIFT_NO; /* unreachable: the table above has no 1d arrangement */
    stq(L, 0, d, q);
    return LIFT_OK;
}


/* Floating point runs. The host FPCR is the default (round to nearest, no flush) everywhere except inside a run: NEON arithmetic runs with the "standard FPSCR"
 * (flush to zero, default NaN, round to nearest) and VFP arithmetic with the guest's own FZ / DN / RMode. A run is opened by the first instruction that needs it and
 * closed before anything that could run host code or leave the straight-line sequence (out-of-line operations, branches, the end of the region), so the
 * runtime's C code (which does its own FPSCR emulation on the host FPU) never sees a modified FPCR. The host FPSR (cumulative exception flags) is saved on
 * entry; a NEON run discards what it accumulated (Advanced SIMD does not touch the guest's flags in this core), a VFP run merges IOC DZC OFC UFC IXC IDC into
 * the guest FPSCR. x25 holds the saved FPSR for the duration of a run. */
static void close_run(Lift *L);
static void begin_run(Lift *L, int kind) {
    if (L->fprun == kind) return;
    if (L->fprun) close_run(L);
    out(L, "\tmrs x25, fpsr");
    if (kind == 1) out(L, "\tmovz x16, #0x300, lsl #16");
    else {
        out(L, "\tmsr fpsr, xzr");
        out(L, "\tldr w16, [x21, #%zu]\n\tand w16, w16, #0x03c00000", offsetof(A32Cpu, fpscr));
    }
    out(L, "\tmsr fpcr, x16");
    L->fprun = kind;
}
static void close_run(Lift *L) {
    if (!L->fprun) return;
    if (L->fprun == 2) {
        out(L, "\tmrs x16, fpsr\n\tmovz w17, #0x9f\n\tand w16, w16, w17\n\tldr w17, [x21, #%zu]\n\torr w17, w17, w16\n\tstr w17, [x21, #%zu]",
            offsetof(A32Cpu, fpscr), offsetof(A32Cpu, fpscr));
    }
    out(L, "\tmsr fpsr, x25\n\tmsr fpcr, xzr");
    L->fprun = 0;
}
static int run_for_flow(const A32Insn *i) {
    switch (i->op) {
        case OP_B:
        case OP_BL:
        case OP_BLX_IMM:
        case OP_BX:
        case OP_BLX_REG:
        case OP_CBZ:
        case OP_TB:
        case OP_SVC:
        case OP_BKPT:
        case OP_UNDEF:
        case OP_UNIMPL: return 1;
        case OP_DP: return i->rd == 15 && !(i->sub >= 8 && i->sub <= 11);
        case OP_LDST: return i->rd == 15;
        case OP_LDM: return (i->imm >> 15) & 1;
        default: return 0;
    }
}

/* NEON floating point, three registers of the same length (single precision lanes) */
static int lift_neon_f3same(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned U = w >> 24 & 1, o = w >> 21 & 1, opc = w >> 8 & 15, c4 = w >> 4 & 1, q = w >> 6 & 1, d = vreg_d(w), n = vreg_n(w), m = vreg_m(w);
    if (opc < 12 || (w >> 20 & 1) || (q && ((d | n | m) & 1))) return LIFT_NO;
    const char *op = NULL;
    int acc = 0, mulacc = 0; /* acc: reads the destination (fused); mulacc: non-fused multiply-accumulate */
    switch (opc) {
        case 12:
            if (!c4 || U) return LIFT_NO;
            op = o ? "fmls" : "fmla";
            acc = 1;
            break;
        case 13:
            if (!c4) {
                if (!U) op = o ? "fsub" : "fadd";
                else if (o) op = "fabd";
                else { if (q) return LIFT_NO; op = "faddp"; }
            } else if (!U) {
                op = o ? "fsub" : "fadd";
                mulacc = 1;
            } else if (!o) op = "fmul";
            else return LIFT_NO;
            break;
        case 14:
            if (!c4) {
                if (!U) {
                    if (o) return LIFT_NO;
                    op = "fcmeq";
                } else op = o ? "fcmgt" : "fcmge";
            } else if (U) op = o ? "facgt" : "facge";
            else return LIFT_NO;
            break;
        default:
            if (!c4 && !U) op = o ? "fmin" : "fmax";
            else if (!c4 && U) { if (q) return LIFT_NO; op = o ? "fminp" : "fmaxp"; }
            else if (c4 && U) op = o ? "fminnm" : "fmaxnm";
            else return LIFT_NO; /* vrecps / vrsqrts are not fused on AArch32: out of line */
            break;
    }
    const char *a = q ? "4s" : "2s";
    begin_run(L, 1);
    vbase(L);
    ldq(L, 1, n, q);
    ldq(L, 2, m, q);
    if (acc || mulacc) ldq(L, 0, d, q);
    if (mulacc) {
        out(L, "\tfmul v3.%s, v1.%s, v2.%s", a, a, a);
        out(L, "\t%s v0.%s, v0.%s, v3.%s", op, a, a, a);
    } else out(L, "\t%s v0.%s, v1.%s, v2.%s", op, a, a, a);
    stq(L, 0, d, q);
    return LIFT_OK;
}

/* VFP data processing (single and double precision) */
static int lift_vfp_dp(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned dbl = w >> 8 & 1, p = w >> 23 & 1, qq = w >> 21 & 1, r = w >> 20 & 1, sbit = w >> 6 & 1;
    unsigned dd = dbl ? vreg_d(w) : ((w >> 12 & 15) << 1 | (w >> 22 & 1)), nn = dbl ? vreg_n(w) : ((w >> 16 & 15) << 1 | (w >> 7 & 1)),
             mm = dbl ? vreg_m(w) : ((w & 15) << 1 | (w >> 5 & 1));
    const char *op = NULL;
    int acc = 0, sqrt = 0;
    if (p == 1 && qq == 1 && r == 1) {
        if ((w & 0x0fbf0ed0u) == 0x0eb10ac0u) sqrt = 1, op = "fsqrt";
        else if ((w & 0x00000ef0u) == 0x00000a00u) { /* vmov Vd, #imm8 (VFPExpandImm) */
            uint32_t imm8 = (w >> 16 & 15) << 4 | (w & 15), sign = imm8 >> 7, b6 = imm8 >> 6 & 1, low = imm8 & 63;
            vbase(L);
            if (dbl) {
                uint64_t v = (uint64_t)sign << 63 | (uint64_t)(!b6) << 62 | (b6 ? 0xffull << 54 : 0) | (uint64_t)low << 48;
                out(L, "\tmovz x16, #0x%llx, lsl #48\n\tmovk x16, #0x%llx, lsl #32", (unsigned long long)(v >> 48), (unsigned long long)(v >> 32 & 0xffff));
                out(L, "\tstr x16, [x15, #%u]", 8 * dd);
            } else {
                uint32_t v = sign << 31 | (!b6) << 30 | (b6 ? 0x1fu << 25 : 0) | low << 19;
                movc(L, "w16", v);
                out(L, "\tstr w16, [x15, #%u]", 4 * dd);
            }
            return LIFT_OK;
        } else return LIFT_NO;
    } else if (!p && !qq && !r) {
        op = sbit ? "fsub" : "fadd";
        acc = 1;
    } else if (!p && qq && !r) op = sbit ? "fnmul" : "fmul";
    else if (!p && qq && r) op = sbit ? "fsub" : "fadd";
    else if (p && !qq && !r && !sbit) op = "fdiv";
    else return LIFT_NO;
    char t = dbl ? 'd' : 's';
    begin_run(L, 2);
    vbase(L);
    unsigned sz = dbl ? 8 : 4;
    out(L, "\tldr %c1, [x15, #%u]", t, sz * mm);
    if (!sqrt) out(L, "\tldr %c2, [x15, #%u]", t, sz * nn);
    if (sqrt) out(L, "\tfsqrt %c0, %c1", t, t);
    else if (acc) {
        out(L, "\tldr %c0, [x15, #%u]", t, sz * dd);
        /* vmls adds the negated product (FPNeg also flips the sign of a NaN product, which a subtraction would not) */
        out(L, "\t%s %c3, %c2, %c1", sbit ? "fnmul" : "fmul", t, t, t);
        out(L, "\tfadd %c0, %c0, %c3", t, t, t);
    } else out(L, "\t%s %c0, %c2, %c1", op, t, t, t);
    out(L, "\tstr %c0, [x15, #%u]", t, sz * dd);
    return LIFT_OK;
}

/* NEON vcvt between 32-bit integers and single precision floats (round toward zero to integer, to nearest to float) */
static int lift_neon_vcvt(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned q = w >> 6 & 1, d = vreg_d(w), m = vreg_m(w), op = w >> 7 & 3;
    if (q && ((d | m) & 1)) return LIFT_NO;
    static const char *const nm[4] = {"scvtf", "ucvtf", "fcvtzs", "fcvtzu"};
    const char *a = q ? "4s" : "2s";
    begin_run(L, 1);
    vbase(L);
    ldq(L, 1, m, q);
    out(L, "\t%s v0.%s, v1.%s", nm[op], a, a);
    stq(L, 0, d, q);
    return LIFT_OK;
}


/* ---- the rarer NEON forms: permutes, tables, duplicates, immediates, shifts, scalars ------------------------------------------------- */
static const char *const ARR[2][4] = {{"8b", "4h", "2s", "1d"}, {"16b", "8h", "4s", "2d"}};
static const char LANE[4] = {'b', 'h', 's', 'd'};

/* AdvSIMDExpandImm: the 64-bit constant of a modified-immediate instruction. Returns 0 if the cmode/op combination is undefined. */
static int neon_expand_imm(uint32_t w, uint64_t *out) {
    unsigned op = w >> 5 & 1, cmode = w >> 8 & 15;
    uint32_t a = (w >> 24 & 1) << 7 | (w >> 16 & 7) << 4 | (w & 15);
    uint64_t v;
    switch (cmode >> 1) {
        case 0: v = a; break;
        case 1: v = (uint64_t)a << 8; break;
        case 2: v = (uint64_t)a << 16; break;
        case 3: v = (uint64_t)a << 24; break;
        case 4: v = a | (uint64_t)a << 32 >> 32 << 16; v = a | (uint64_t)a << 16; break;
        case 5: v = ((uint64_t)a << 8) | ((uint64_t)a << 24); break;
        case 6: v = (cmode & 1) ? (uint64_t)a << 16 | 0xffff : (uint64_t)a << 8 | 0xff; break;
        default:
            if (!(cmode & 1) && !op) v = a | (uint64_t)a << 8 | (uint64_t)a << 16 | (uint64_t)a << 24;
            else if (!(cmode & 1) && op) {
                v = 0;
                for (int i = 0; i < 8; i++)
                    if (a >> i & 1) v |= 0xffull << (8 * i);
                *out = v;
                return 1;
            } else if ((cmode & 1) && !op) {
                uint32_t f = (a & 0x80) << 24 | ((a & 0x40) ? 0x3e000000u : 0x40000000u) | (a & 0x3f) << 19;
                v = f | (uint64_t)f << 32;
                *out = v;
                return 1;
            } else return 0;
            v |= v << 32;
            *out = v;
            return 1;
    }
    if ((cmode >> 1) <= 3) v |= v << 32;
    else if ((cmode >> 1) <= 5) v = (uint16_t)v | (v & 0xffff) << 16 | (v & 0xffff) << 32 | (v & 0xffff) << 48;
    else v |= v << 32;
    *out = v;
    return 1;
}

static void mov64(Lift *L, const char *x, uint64_t v) {
    out(L, "\tmovz %s, #0x%llx", x, (unsigned long long)(v & 0xffff));
    for (int i = 1; i < 4; i++)
        if ((v >> (16 * i)) & 0xffff) out(L, "\tmovk %s, #0x%llx, lsl #%d", x, (unsigned long long)((v >> (16 * i)) & 0xffff), 16 * i);
}

static int lift_neon_modimm(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned op = w >> 5 & 1, cmode = w >> 8 & 15, q = w >> 6 & 1, d = vreg_d(w);
    uint64_t v;
    if (!neon_expand_imm(w, &v)) return LIFT_NO;
    if (q && (d & 1)) return LIFT_NO;
    int bitop = 0; /* 0 vmov, 1 vmvn, 2 vorr, 3 vbic */
    if ((cmode & 9) == 1 || (cmode & 13) == 9) bitop = op ? 3 : 2; /* 0xx1 and 10x1 */
    else if (op && cmode != 14 && cmode != 15) bitop = 1;
    else if (op && cmode == 14) bitop = 0; /* the byte-mask form: expand_imm already produced the value */
    else if (op) return LIFT_NO;
    if (bitop == 1) v = ~v;
    vbase(L);
    mov64(L, "x16", v);
    out(L, "\tdup v2.2d, x16");
    if (bitop >= 2) {
        ldq(L, 1, d, q);
        out(L, "\t%s v0.%s, v1.%s, v2.%s", bitop == 2 ? "orr" : "bic", ARR[q][0], ARR[q][0], ARR[q][0]);
        stq(L, 0, d, q);
    } else stq(L, 2, d, q);
    return LIFT_OK;
}

/* loads D registers n..n+cnt-1 into v1 (and v2) as one or two 16-byte table registers; the unused upper bytes are zero (tbl then returns 0 for them) */
static void ld_table(Lift *L, unsigned n, unsigned cnt) {
    for (unsigned k = 0; k < cnt; k += 2) {
        unsigned v = 1 + k / 2;
        if (cnt - k >= 2 && !((n + k) & 1)) out(L, "\tldr q%u, [x15, #%u]", v, 8 * (n + k));
        else if (cnt - k >= 2) out(L, "\tldr d%u, [x15, #%u]\n\tldr d3, [x15, #%u]\n\tmov v%u.d[1], v3.d[0]", v, 8 * (n + k), 8 * (n + k + 1), v);
        else out(L, "\tldr d%u, [x15, #%u]", v, 8 * (n + k));
    }
}

static int lift_neon_ext_tbl_dup(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned d = vreg_d(w), n = vreg_n(w), m = vreg_m(w), q = w >> 6 & 1;
    if (!(w >> 23 & 1) || (w >> 20 & 3) != 3) return LIFT_NO;
    if (!(w >> 24 & 1)) { /* vext */
        if ((w & 0xffb00010u) != 0xf2b00000u) return LIFT_NO;
        unsigned imm = w >> 8 & 15;
        if (q && ((d | n | m) & 1)) return LIFT_NO;
        if (!q && imm >= 8) return LIFT_NO;
        vbase(L);
        ldq(L, 1, n, q);
        ldq(L, 2, m, q);
        out(L, "\text v0.%s, v1.%s, v2.%s, #%u", ARR[q][0], ARR[q][0], ARR[q][0], imm);
        stq(L, 0, d, q);
        return LIFT_OK;
    }
    if ((w & 0xffb00c10u) == 0xf3b00800u) { /* vtbl / vtbx */
        unsigned len = (w >> 8 & 3) + 1, x = w >> 6 & 1;
        if (n + len > 32) return LIFT_NO;
        vbase(L);
        ld_table(L, n, len);
        out(L, "\tldr d4, [x15, #%u]", 8 * m);
        if (x) out(L, "\tldr d0, [x15, #%u]", 8 * d);
        const char *tab = len <= 2 ? "{v1.16b}" : "{v1.16b, v2.16b}";
        if (x && (len & 1)) {
            /* vtbx leaves the destination byte unchanged for an index >= 8 * len; the zero-extended half of the last table register would make A64 tbx
             * treat those indices as in range, so select explicitly: result = index < 8 * len ? tbl : old */
            out(L, "\ttbl v3.8b, %s, v4.8b", tab);
            out(L, "\tmovi v6.8b, #%u", 8 * len);
            out(L, "\tcmhi v5.8b, v6.8b, v4.8b");
            out(L, "\tbsl v5.8b, v3.8b, v0.8b");
            out(L, "\tmov v0.8b, v5.8b");
        } else out(L, "\t%s v0.8b, %s, v4.8b", x ? "tbx" : "tbl", tab);
        out(L, "\tstr d0, [x15, #%u]", 8 * d);
        return LIFT_OK;
    }
    if ((w & 0xffb00f90u) == 0xf3b00c00u) { /* vdup scalar */
        unsigned imm4 = w >> 16 & 15, sz, idx;
        if (imm4 & 1) sz = 0, idx = imm4 >> 1;
        else if (imm4 & 2) sz = 1, idx = imm4 >> 2;
        else if (imm4 & 4) sz = 2, idx = imm4 >> 3;
        else return LIFT_NO;
        if (q && (d & 1)) return LIFT_NO;
        vbase(L);
        out(L, "\tldr d1, [x15, #%u]", 8 * m);
        out(L, "\tdup v0.%s, v1.%c[%u]", ARR[q][sz], LANE[sz], idx);
        stq(L, 0, d, q);
        return LIFT_OK;
    }
    return LIFT_NO;
}

/* two registers, miscellaneous: 1111 0011 1D11 size 1x Vd 0 opc Q M 0 Vm */
static int lift_neon_2misc(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned sz = w >> 18 & 3, B = w >> 16 & 3, opc = w >> 7 & 15, q = w >> 6 & 1, d = vreg_d(w), m = vreg_m(w);
    if (q && ((d | m) & 1)) return LIFT_NO;
    const char *a = ARR[q][sz];
    switch (B) {
        case 0:
            if (opc <= 2) { /* vrev64 / vrev32 / vrev16 */
                static const char *const rv[3] = {"rev64", "rev32", "rev16"};
                if (sz + opc >= 3) return LIFT_NO;
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\t%s v0.%s, v1.%s", rv[opc], a, a);
                stq(L, 0, d, q);
                return LIFT_OK;
            }
            if (opc == 11) { /* vmvn */
                if (sz) return LIFT_NO;
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\tnot v0.%s, v1.%s", a, a);
                stq(L, 0, d, q);
                return LIFT_OK;
            }
            if (opc == 10) { /* vcnt */
                if (sz) return LIFT_NO;
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\tcnt v0.%s, v1.%s", a, a);
                stq(L, 0, d, q);
                return LIFT_OK;
            }
            if (opc == 8 || opc == 9) { /* vcls / vclz */
                if (sz == 3) return LIFT_NO;
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\t%s v0.%s, v1.%s", opc == 8 ? "cls" : "clz", a, a);
                stq(L, 0, d, q);
                return LIFT_OK;
            }
            return LIFT_NO;
        case 1: {
            unsigned F = opc >> 3 & 1, o = opc & 7;
            if (F && sz != 2) return LIFT_NO;
            if (!F && sz == 3) return LIFT_NO;
            if (o == 5) return LIFT_NO;
            if (!F) {
                static const char *const nm[8] = {"cmgt", "cmge", "cmeq", "cmle", "cmlt", "", "abs", "neg"};
                vbase(L);
                ldq(L, 1, m, q);
                if (o <= 4) out(L, "\t%s v0.%s, v1.%s, #0", nm[o], a, a);
                else out(L, "\t%s v0.%s, v1.%s", nm[o], a, a);
                stq(L, 0, d, q);
                return LIFT_OK;
            }
            static const char *const fn[8] = {"fcmgt", "fcmge", "fcmeq", "fcmle", "fcmlt", "", "fabs", "fneg"};
            if (o >= 6) { /* sign bit operations: no FP state */
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\t%s v0.%s, v1.%s", fn[o], a, a);
            } else {
                begin_run(L, 1);
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\t%s v0.%s, v1.%s, #0.0", fn[o], a, a);
            }
            stq(L, 0, d, q);
            return LIFT_OK;
        }
        case 2:
            if (opc == 0) { /* vswp */
                if (sz) return LIFT_NO;
                vbase(L);
                ldq(L, 1, m, q);
                ldq(L, 2, d, q);
                stq(L, 1, d, q);
                stq(L, 2, m, q);
                return LIFT_OK;
            }
            if (opc >= 1 && opc <= 3) { /* vtrn / vuzp / vzip: both registers are results */
                static const char *const p1[4] = {"", "trn1", "uzp1", "zip1"}, *const p2[4] = {"", "trn2", "uzp2", "zip2"};
                if (sz == 3 || (opc != 1 && sz == 2 && !q)) return LIFT_NO;
                if (d == m) return LIFT_NO;
                vbase(L);
                ldq(L, 1, d, q);
                ldq(L, 2, m, q);
                out(L, "\t%s v3.%s, v1.%s, v2.%s", p1[opc], a, a, a);
                out(L, "\t%s v4.%s, v1.%s, v2.%s", p2[opc], a, a, a);
                stq(L, 3, d, q);
                stq(L, 4, m, q);
                return LIFT_OK;
            }
            if (opc == 4 && !q) { /* vmovn */
                if (sz == 3 || (m & 1)) return LIFT_NO;
                vbase(L);
                ldq(L, 1, m, 1);
                out(L, "\txtn v0.%s, v1.%s", ARR[0][sz], ARR[1][sz + 1]);
                out(L, "\tstr d0, [x15, #%u]", 8 * d);
                return LIFT_OK;
            }
            if (opc == 6 && !q) { /* vshll by the element size */
                if (sz == 3 || (d & 1)) return LIFT_NO;
                vbase(L);
                out(L, "\tldr d1, [x15, #%u]", 8 * m);
                out(L, "\tshll v0.%s, v1.%s, #%u", ARR[1][sz + 1], ARR[0][sz], 8u << sz);
                stq(L, 0, d, 1);
                return LIFT_OK;
            }
            return LIFT_NO;
        default: {
            if (sz != 2) return LIFT_NO;
            unsigned op = opc >> 1 & 7, lo = opc & 1; /* bits 10:7 = op(3) lo? no: opc = bits 10:7, see below */
            (void)op;
            (void)lo;
            unsigned o4 = opc; /* 0..7 vcvta/n/p/m (s/u), 8/9 recpe (u/f), 10/11 rsqrte (u/f), 12..15 vcvt */
            if (o4 >= 12) {
                static const char *const cv[4] = {"scvtf", "ucvtf", "fcvtzs", "fcvtzu"};
                begin_run(L, 1);
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\t%s v0.%s, v1.%s", cv[o4 - 12], a, a);
                stq(L, 0, d, q);
                return LIFT_OK;
            }
            if (o4 < 8) { /* vcvt{a,n,p,m}.{s,u}32.f32 */
                static const char *const rm[4] = {"a", "n", "p", "m"};
                begin_run(L, 1);
                vbase(L);
                ldq(L, 1, m, q);
                out(L, "\tfcvt%s%c v0.%s, v1.%s", rm[o4 >> 1], (o4 & 1) ? 'u' : 's', a, a);
                stq(L, 0, d, q);
                return LIFT_OK;
            }
            return LIFT_NO; /* vrecpe / vrsqrte: table estimates, out of line */
        }
    }
}

/* shifts by immediate: 1111 001U 1D imm6 Vd opc L Q M 1 Vm (bits 21:19 of imm6 non-zero, or L) */
static int lift_neon_shift(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned U = w >> 24 & 1, imm6 = w >> 16 & 63, Lb = w >> 7 & 1, opc = w >> 8 & 15, q = w >> 6 & 1, d = vreg_d(w), m = vreg_m(w);
    unsigned sz, esz;
    if (Lb) sz = 3;
    else if (imm6 & 32) sz = 2;
    else if (imm6 & 16) sz = 1;
    else if (imm6 & 8) sz = 0;
    else return LIFT_NO;
    esz = 8u << sz;
    if (opc == 10 && !q) { /* vshll / vmovl: the size is the narrow one */
        if (Lb || (d & 1)) return LIFT_NO;
        unsigned sh = imm6 - esz;
        vbase(L);
        out(L, "\tldr d1, [x15, #%u]", 8 * m);
        out(L, "\t%sshll v0.%s, v1.%s, #%u", U ? "u" : "s", ARR[1][sz + 1], ARR[0][sz], sh);
        stq(L, 0, d, 1);
        return LIFT_OK;
    }
    if (opc == 8) { /* vshrn / vrshrn (U = 0; bit 6 = round), vqshrun / vqrshrun (U = 1): QC, out of line */
        if (U || Lb || (m & 1) || (w >> 7 & 1)) return LIFT_NO;
        unsigned sh = 2 * esz - imm6; /* the size is the narrow one (sz from imm6 is the narrow size) */
        vbase(L);
        ldq(L, 1, m, 1);
        out(L, "\t%s v0.%s, v1.%s, #%u", (w & 0x40) ? "rshrn" : "shrn", ARR[0][sz], ARR[1][sz + 1], sh);
        out(L, "\tstr d0, [x15, #%u]", 8 * d);
        return LIFT_OK;
    }
    if (q && ((d | m) & 1)) return LIFT_NO;
    const char *a = ARR[q][sz], *op = NULL;
    unsigned sh, right = (sz == 3 ? 64 : 2 * esz) - imm6, left = sz == 3 ? imm6 : imm6 - esz; /* with L set the whole imm6 is the amount */
    int acc = 0;
    switch (opc) {
        case 0: op = U ? "ushr" : "sshr"; sh = right; break;
        case 1: op = U ? "usra" : "ssra"; sh = right; acc = 1; break;
        case 2: op = U ? "urshr" : "srshr"; sh = right; break;
        case 3: op = U ? "ursra" : "srsra"; sh = right; acc = 1; break;
        case 4: if (!U) return LIFT_NO; op = "sri"; sh = right; acc = 1; break;
        case 5: op = U ? "sli" : "shl"; sh = left; acc = U; break;
        default: return LIFT_NO; /* saturating shifts set QC; vcvt fixed point: out of line */
    }
    if (sz == 3 && !q) a = "1d"; /* 64-bit shifts on a D register: the 1d arrangement is accepted for these */
    if (sz == 3 && !q) {
        vbase(L);
        out(L, "\tldr d1, [x15, #%u]", 8 * m);
        if (acc) out(L, "\tldr d0, [x15, #%u]", 8 * d);
        out(L, "\t%s d0, d1, #%u", op, sh);
        out(L, "\tstr d0, [x15, #%u]", 8 * d);
        return LIFT_OK;
    }
    vbase(L);
    ldq(L, 1, m, q);
    if (acc) ldq(L, 0, d, q);
    out(L, "\t%s v0.%s, v1.%s, #%u", op, a, a, sh);
    stq(L, 0, d, q);
    return LIFT_OK;
}

/* two registers and a scalar: 1111 001Q 1D sz Vn Vd opc N 1 M 0 Vm */
static int lift_neon_scalar(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned q = w >> 24 & 1, sz = w >> 20 & 3, opc = w >> 8 & 15, d = vreg_d(w), n = vreg_n(w);
    unsigned m, idx;
    if (sz == 1) {
        m = w & 7;
        idx = (w >> 5 & 1) << 1 | (w >> 3 & 1);
    } else if (sz == 2) {
        m = w & 15;
        idx = w >> 5 & 1;
    } else return LIFT_NO;
    if (q && ((d | n) & 1)) return LIFT_NO;
    const char *a = ARR[q][sz];
    int F = opc & 1;
    if (F && sz != 2) return LIFT_NO;
    switch (opc & 14) {
        case 0: /* vmla */
        case 4: /* vmls */
        case 8: /* vmul */ break;
        default: return LIFT_NO; /* long and saturating forms: out of line */
    }
    if (F) begin_run(L, 1);
    vbase(L);
    ldq(L, 1, n, q);
    out(L, "\tldr d2, [x15, #%u]", 8 * m);
    if ((opc & 14) == 8) {
        out(L, "\t%s v0.%s, v1.%s, v2.%c[%u]", F ? "fmul" : "mul", a, a, LANE[sz], idx);
    } else {
        ldq(L, 0, d, q);
        if (F) { /* not fused on AArch32 */
            out(L, "\tfmul v3.%s, v1.%s, v2.%c[%u]", a, a, LANE[sz], idx);
            out(L, "\t%s v0.%s, v0.%s, v3.%s", (opc & 4) ? "fsub" : "fadd", a, a, a);
        } else out(L, "\t%s v0.%s, v1.%s, v2.%c[%u]", (opc & 4) ? "mls" : "mla", a, a, LANE[sz], idx);
    }
    stq(L, 0, d, q);
    return LIFT_OK;
}

/* vdup (core register) and the scalar <-> core register moves, cp10 register-transfer encodings */
static int lift_vfp_scalar_moves(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned rt = w >> 12 & 15;
    if (rt == 15) return LIFT_NO;
    if ((w & 0x0f900f90u) == 0x0e800b10u) { /* vdup Dd / Qd, Rt (bit 22 = B, bit 21 = Q) */
        unsigned b = w >> 22 & 1, e = w >> 5 & 1, q = w >> 21 & 1, d = (w >> 7 & 1) << 4 | (w >> 16 & 15);
        unsigned sz = b ? (e ? 3 : 0) : (e ? 1 : 2);
        if (sz == 3 || (q && (d & 1))) return LIFT_NO;
        vbase(L);
        out(L, "\tdup v0.%s, %s", ARR[q][sz], W[rt]);
        stq(L, 0, d, q);
        return LIFT_OK;
    }
    if ((w & 0x0f900f1fu) == 0x0e000b10u || (w & 0x0f100f1fu) == 0x0e100b10u) { /* vmov Dd[x], Rt  /  vmov Rt, Dn[x] */
        unsigned load = w >> 20 & 1, u = w >> 23 & 1, opc1 = w >> 21 & 3, opc2 = w >> 5 & 3, d = (w >> 7 & 1) << 4 | (w >> 16 & 15);
        unsigned sz, idx;
        if (opc1 & 2) sz = 0, idx = (opc1 & 1) << 2 | opc2;
        else if (opc2 & 1) sz = 1, idx = (opc1 & 1) << 1 | (opc2 >> 1);
        else if (opc2 == 0) sz = 2, idx = opc1 & 1;
        else return LIFT_NO;
        if (!load && u) return LIFT_NO;
        if (load && sz == 2 && u) return LIFT_NO;
        vbase(L);
        out(L, "\tldr d0, [x15, #%u]", 8 * d);
        if (load) out(L, "\t%s %s, v0.%c[%u]", sz == 2 ? "mov" : u ? "umov" : "smov", W[rt], LANE[sz], idx);
        else {
            out(L, "\tmov v0.%c[%u], %s", LANE[sz], idx, W[rt]);
            out(L, "\tstr d0, [x15, #%u]", 8 * d);
        }
        return LIFT_OK;
    }
    return LIFT_NO;
}

/* ARMv8 VFP additions (unconditional, 1111 1110 ...): vsel, vmaxnm / vminnm, vrint{a,n,p,m}, vcvt{a,n,p,m} */
static int lift_vfp_v8(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned dbl = w >> 8 & 1;
    char t = dbl ? 'd' : 's';
    unsigned sz = dbl ? 8 : 4;
    unsigned dd = dbl ? vreg_d(w) : ((w >> 12 & 15) << 1 | (w >> 22 & 1)), nn = dbl ? vreg_n(w) : ((w >> 16 & 15) << 1 | (w >> 7 & 1)),
             mm = dbl ? vreg_m(w) : ((w & 15) << 1 | (w >> 5 & 1));
    if ((w & 0xff800e50u) == 0xfe000a00u) { /* vsel<cc> */
        static const char *const cc[4] = {"eq", "vs", "ge", "gt"};
        vbase(L);
        out(L, "\tldr %c1, [x15, #%u]\n\tldr %c2, [x15, #%u]", t, sz * nn, t, sz * mm);
        out(L, "\tfcsel %c0, %c1, %c2, %s", t, t, t, cc[w >> 20 & 3]);
        out(L, "\tstr %c0, [x15, #%u]", t, sz * dd);
        return LIFT_OK;
    }
    if ((w & 0xffb00e10u) == 0xfe800a00u) { /* vmaxnm / vminnm (bit 6 selects min) */
        begin_run(L, 2);
        vbase(L);
        out(L, "\tldr %c1, [x15, #%u]\n\tldr %c2, [x15, #%u]", t, sz * nn, t, sz * mm);
        out(L, "\t%s %c0, %c1, %c2", (w >> 6 & 1) ? "fminnm" : "fmaxnm", t, t, t);
        out(L, "\tstr %c0, [x15, #%u]", t, sz * dd);
        return LIFT_OK;
    }
    if ((w & 0xffbc0ed0u) == 0xfeb80a40u) { /* vrint{a,n,p,m} */
        static const char *const rm[4] = {"a", "n", "p", "m"};
        begin_run(L, 2);
        vbase(L);
        out(L, "\tldr %c1, [x15, #%u]", t, sz * mm);
        out(L, "\tfrint%s %c0, %c1", rm[w >> 16 & 3], t, t);
        out(L, "\tstr %c0, [x15, #%u]", t, sz * dd);
        return LIFT_OK;
    }
    if ((w & 0xffbc0e50u) == 0xfebc0a40u) { /* vcvt{a,n,p,m}.{s,u}32 */
        static const char *const rm[4] = {"a", "n", "p", "m"};
        unsigned sd = (w >> 12 & 15) << 1 | (w >> 22 & 1);
        begin_run(L, 2);
        vbase(L);
        out(L, "\tldr %c1, [x15, #%u]", t, sz * mm);
        out(L, "\tfcvt%s%c w16, %c1", rm[w >> 16 & 3], (w >> 7 & 1) ? 's' : 'u', t);
        out(L, "\tstr w16, [x15, #%u]", 4 * sd);
        return LIFT_OK;
    }
    return LIFT_NO;
}

/* vrintz / vrintx / vrintr (conditional ARMv8 VFP), vcvtr (convert with the FPSCR rounding mode) */
static int lift_vfp_rint(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned dbl = w >> 8 & 1;
    char t = dbl ? 'd' : 's';
    unsigned sz = dbl ? 8 : 4;
    unsigned dd = dbl ? vreg_d(w) : ((w >> 12 & 15) << 1 | (w >> 22 & 1)), mm = dbl ? vreg_m(w) : ((w & 15) << 1 | (w >> 5 & 1));
    if ((w & 0x0fbf0e50u) == 0x0eb60a40u || (w & 0x0fbf0ed0u) == 0x0eb70a40u) { /* opc2 110: vrintr (bit 7 clear) / vrintz (set); opc2 111 with bit 7 clear: vrintx */
        const char *op = (w >> 16 & 1) ? "frintx" : (w >> 7 & 1) ? "frintz" : "frinti";
        begin_run(L, 2);
        vbase(L);
        out(L, "\tldr %c1, [x15, #%u]\n\t%s %c0, %c1\n\tstr %c0, [x15, #%u]", t, sz * mm, op, t, t, t, sz * dd);
        return LIFT_OK;
    }
    if ((w & 0x0fbe0e50u) == 0x0ebc0a40u && !(w >> 7 & 1)) { /* vcvtr.{s,u}32: round per FPSCR, then truncate */
        unsigned sd = (w >> 12 & 15) << 1 | (w >> 22 & 1), sgn = w >> 16 & 1;
        begin_run(L, 2);
        vbase(L);
        out(L, "\tldr %c1, [x15, #%u]\n\tfrinti %c1, %c1\n\t%s w16, %c1\n\tstr w16, [x15, #%u]", t, sz * mm, t, t, sgn ? "fcvtzs" : "fcvtzu", t, 4 * sd);
        return LIFT_OK;
    }
    return LIFT_NO;
}

/* the rest of the three-same group that maps directly: integer vpadd, vpmax / vpmin, vhadd / vhsub / vrhadd, vabd / vaba, vmla / vmls, vqadd / vqsub are left out (QC) */
static int lift_neon_3same_more(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned U = w >> 24 & 1, size = w >> 20 & 3, opc = w >> 8 & 15, c4 = w >> 4 & 1, q = w >> 6 & 1, d = vreg_d(w), n = vreg_n(w), m = vreg_m(w);
    if (q && ((d | n | m) & 1)) return LIFT_NO;
    if (size == 3) return LIFT_NO;
    const char *a = ARR[q][size], *op = NULL;
    int acc = 0;
    switch (opc) {
        case 0: op = c4 ? (U ? "uqadd" : "sqadd") : (U ? "uhadd" : "shadd"); if (c4) return LIFT_NO; break;
        case 1: if (c4) return LIFT_NO; op = U ? "urhadd" : "srhadd"; break;
        case 2: if (c4) return LIFT_NO; op = U ? "uhsub" : "shsub"; break;
        case 7: op = U ? "uabd" : "sabd"; if (c4) { op = U ? "uaba" : "saba"; acc = 1; } break;
        case 9: if (!c4) { op = U ? "mls" : "mla"; acc = 1; } else return LIFT_NO; break;
        case 10: if (q) return LIFT_NO; op = c4 ? (U ? "uminp" : "sminp") : (U ? "umaxp" : "smaxp"); break;
        case 11:
            if (c4 && !U) { if (q) return LIFT_NO; op = "addp"; }
            else return LIFT_NO; /* vqdmulh / vqrdmulh: QC */
            break;
        default: return LIFT_NO;
    }
    vbase(L);
    ldq(L, 1, n, q);
    ldq(L, 2, m, q);
    if (acc) ldq(L, 0, d, q);
    out(L, "\t%s v0.%s, v1.%s, v2.%s", op, a, a, a);
    stq(L, 0, d, q);
    return LIFT_OK;
}

static int lift_neon(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm;
    unsigned mk = inline_mask();
    if ((w & 0xff000000u) == 0xf4000000u) return (mk & 1) ? lift_neon_ls(L, i) : LIFT_NO;
    if ((w & 0xfe000000u) != 0xf2000000u) return LIFT_NO;
    unsigned size = w >> 20 & 3;
    if (!(w >> 23 & 1)) { /* three registers of the same length */
        int r = (mk & 2) ? lift_neon_3same(L, i) : LIFT_NO;
        if (r == LIFT_NO && (mk & 2)) r = lift_neon_3same_more(L, i);
        return r != LIFT_NO ? r : (mk & 4) ? lift_neon_f3same(L, i) : LIFT_NO;
    }
    if (w >> 4 & 1) {
        if ((w >> 19 & 7) == 0 && !(w >> 7 & 1)) return (mk & 2) ? lift_neon_modimm(L, i) : LIFT_NO;
        return (mk & 2) ? lift_neon_shift(L, i) : LIFT_NO;
    }
    if (size != 3) return (w >> 6 & 1) ? ((mk & 2) ? lift_neon_scalar(L, i) : LIFT_NO) : LIFT_NO; /* three different lengths: out of line */
    if (!(w >> 24 & 1)) return (mk & 2) ? lift_neon_ext_tbl_dup(L, i) : LIFT_NO;
    if (!(w >> 11 & 1)) return (mk & 6) ? lift_neon_2misc(L, i) : LIFT_NO;
    return (mk & 2) ? lift_neon_ext_tbl_dup(L, i) : LIFT_NO;
}

static int lift_vfp(Lift *L, const A32Insn *i) {
    uint32_t w = i->imm, pcv = L->pc + (L->thumb ? 4 : 8);
    unsigned rn = w >> 16 & 15, rt = w >> 12 & 15;
    unsigned mk = inline_mask();
    if (w >> 28 == 15) return (mk & 32) ? lift_vfp_v8(L, i) : LIFT_NO;
    if ((w & 0x0f000f10u) == 0x0e000b10u && (mk & 8)) {
        int r = lift_vfp_scalar_moves(L, i);
        if (r != LIFT_NO) return r;
    }
    if ((mk & 32)) {
        int r = lift_vfp_rint(L, i);
        if (r != LIFT_NO) return r;
    }
    if ((w & 0x0f000e10u) == 0x0e000a00u && (mk & 16)) {
        int r = lift_vfp_dp(L, i);
        if (r != LIFT_NO) return r;
    }

    if (((w & 0x0fbf0e50u) == 0x0eb80a40u || (w & 0x0fbe0e50u) == 0x0ebc0a40u) &&
        (mk & 32)) { /* vcvt int <-> float (RZ to integer), cond 11101 D11 1opc2 Vd 101 sz op 1 M 0 Vm */
        unsigned dbl = w >> 8 & 1, op = w >> 7 & 1, to_int = (w >> 18 & 1), sgn;
        if (to_int) {
            if (!op) return LIFT_NO; /* vcvtr: rounds with FPSCR.RMode, out of line */
            sgn = w >> 16 & 1;
        } else {
            sgn = op;
        }
        unsigned sd = (w >> 12 & 15) << 1 | (w >> 22 & 1), sm = (w & 15) << 1 | (w >> 5 & 1);
        unsigned dd = vreg_d(w), dm = vreg_m(w);
        begin_run(L, 2);
        vbase(L);
        if (to_int) {
            out(L, "\tldr %c1, [x15, #%u]\n\t%s w16, %c1\n\tstr w16, [x15, #%u]", dbl ? 'd' : 's', dbl ? 8 * dm : 4 * sm, sgn ? "fcvtzs" : "fcvtzu",
                dbl ? 'd' : 's', 4 * sd);
        } else {
            out(L, "\tldr w16, [x15, #%u]\n\t%s %c0, w16\n\tstr %c0, [x15, #%u]", 4 * sm, sgn ? "scvtf" : "ucvtf", dbl ? 'd' : 's', dbl ? 'd' : 's',
                dbl ? 8 * dd : 4 * sd);
        }
        return LIFT_OK;
    }

    if (((w & 0x0fbf0e50u) == 0x0eb40a40u || (w & 0x0fbf0e50u) == 0x0eb50a40u) &&
        (mk & 32)) { /* vcmp / vcmpe Vd, Vm and Vd, #0.0: the result goes into FPSCR N Z C V */
        unsigned dbl = w >> 8 & 1, zero = w >> 16 & 1, e = w >> 7 & 1;
        unsigned dd = dbl ? vreg_d(w) : ((w >> 12 & 15) << 1 | (w >> 22 & 1)), mm = dbl ? vreg_m(w) : ((w & 15) << 1 | (w >> 5 & 1));
        char t = dbl ? 'd' : 's';
        unsigned sz = dbl ? 8 : 4;
        begin_run(L, 2);
        vbase(L);
        out(L, "\tldr %c1, [x15, #%u]", t, sz * dd);
        if (!zero) out(L, "\tldr %c2, [x15, #%u]", t, sz * mm);
        out(L, "\tmrs x16, nzcv"); /* the guest's own flags live in the host NZCV */
        if (zero) out(L, "\t%s %c1, #0.0", e ? "fcmpe" : "fcmp", t);
        else out(L, "\t%s %c1, %c2", e ? "fcmpe" : "fcmp", t, t);
        out(L, "\tmrs x17, nzcv\n\tmsr nzcv, x16\n\tand w17, w17, #0xf0000000");
        out(L, "\tldr w16, [x21, #%zu]\n\tand w16, w16, #0x0fffffff\n\torr w16, w16, w17\n\tstr w16, [x21, #%zu]", offsetof(A32Cpu, fpscr),
            offsetof(A32Cpu, fpscr));
        return LIFT_OK;
    }
    if ((w & 0x0fff0fffu) == 0x0ef10a10u && rt != 15) { /* vmrs Rt, fpscr: the cumulative bits of an open run are merged first */
        close_run(L);
        out(L, "\tldr %s, [x21, #%zu]", W[rt], offsetof(A32Cpu, fpscr));
        return LIFT_OK;
    }
    if ((w & 0x0fff0fffu) == 0x0ee10a10u && rt != 15) { /* vmsr fpscr, Rt: the next run reloads the modes from it */
        close_run(L);
        movc(L, "w16", 0xffc8ffffu); /* the reserved bits read as zero */
        out(L, "\tand w16, w16, %s", W[rt]);
        out(L, "\tstr w16, [x21, #%zu]", offsetof(A32Cpu, fpscr));
        return LIFT_OK;
    }
    if ((w & 0x0fffffffu) == 0x0ef1fa10u) { /* vmrs APSR_nzcv, fpscr */
        out(L, "\tldr w16, [x21, #%zu]\n\tand w16, w16, #0xf0000000\n\tmsr nzcv, x16", offsetof(A32Cpu, fpscr));
        return LIFT_OK;
    }
    if ((w & 0x0fbf0ed0u) == 0x0eb70ac0u && (mk & 32)) { /* vcvt.f64.f32 / vcvt.f32.f64 */
        unsigned dbl = w >> 8 & 1;
        unsigned sd = (w >> 12 & 15) << 1 | (w >> 22 & 1), sm = (w & 15) << 1 | (w >> 5 & 1);
        begin_run(L, 2);
        vbase(L);
        if (dbl) out(L, "\tldr d1, [x15, #%u]\n\tfcvt s0, d1\n\tstr s0, [x15, #%u]", 8 * vreg_m(w), 4 * sd);
        else out(L, "\tldr s1, [x15, #%u]\n\tfcvt d0, s1\n\tstr d0, [x15, #%u]", 4 * sm, 8 * vreg_d(w));
        return LIFT_OK;
    }
    if (!(mk & 8)) return LIFT_NO;
    if ((w & 0x0f200e00u) == 0x0d000a00u) { /* vldr / vstr (cond 1101 UD0L Rn Vd 101x imm8) */
        unsigned dbl = w >> 8 & 1, up = w >> 23 & 1, load = w >> 20 & 1, imm = (w & 255) * 4;
        unsigned reg = dbl ? vreg_d(w) : ((w >> 12 & 15) << 1 | (w >> 22 & 1));
        if (rn == 15) {
            movc(L, "w16", (pcv & ~3u) + (up ? imm : (uint32_t)-(int32_t)imm));
        } else out(L, "\t%s w16, %s, #%u", up ? "add" : "sub", W[rn], imm);
        vbase(L);
        const char *r = dbl ? "d0" : "s0";
        if (load) out(L, "\tldr %s, [x20, w16, uxtw]\n\tstr %s, [x15, #%u]", r, r, (dbl ? 8 : 4) * reg);
        else out(L, "\tldr %s, [x15, #%u]\n\tstr %s, [x20, w16, uxtw]", r, (dbl ? 8 : 4) * reg, r);
        return LIFT_OK;
    }
    if ((w & 0x0e000e00u) == 0x0c000a00u && !((w >> 23 & 3) == 0 && !(w >> 21 & 1))) {
        /* (P, U, W) = 0 is the 64-bit transfer space (vmov two registers) */ /* vldm / vstm / vpush / vpop (cond 110 PUDWL Rn Vd 101x imm8) */
        unsigned p = w >> 24 & 1, u = w >> 23 & 1, wb = w >> 21 & 1, load = w >> 20 & 1, dbl = w >> 8 & 1, cnt = w & 255;
        if ((p && !wb) || (p == u) || rn == 15) return LIFT_NO; /* vldr/vstr space, and the unallocated P == U */
        unsigned reg = dbl ? vreg_d(w) : ((w >> 12 & 15) << 1 | (w >> 22 & 1));
        unsigned bytes = cnt * 4;
        if (!cnt || bytes > 256 || (dbl && ((cnt & 1) || reg + cnt / 2 > 32)) || (!dbl && reg + cnt > 32)) return LIFT_NO;
        vbase(L);
        if (p) out(L, "\tsub w16, %s, #%u", W[rn], bytes); /* decrement before */
        else out(L, "\tmov w16, %s", W[rn]);
        out(L, "\tadd x17, x20, w16, uxtw");
        unsigned off = (dbl ? 8 : 4) * reg;
        for (unsigned k = 0; k < bytes;) {
            unsigned chunk = bytes - k >= 16 ? 16 : bytes - k >= 8 ? 8 : 4;
            const char *r = chunk == 16 ? "q0" : chunk == 8 ? "d0" : "s0";
            if (load) out(L, "\tldur %s, [x17, #%u]\n\tstur %s, [x15, #%u]", r, k, r, off + k);
            else out(L, "\tldur %s, [x15, #%u]\n\tstur %s, [x17, #%u]", r, off + k, r, k);
            k += chunk;
        }
        if (wb) out(L, "\t%s %s, %s, #%u", u ? "add" : "sub", W[rn], W[rn], bytes);
        return LIFT_OK;
    }
    if ((w & 0x0fe00f7fu) == 0x0e000a10u && rt < 15) { /* vmov Sn, Rt / vmov Rt, Sn */
        unsigned sn = (w >> 16 & 15) << 1 | (w >> 7 & 1);
        vbase(L);
        if (w >> 20 & 1) out(L, "\tldr %s, [x15, #%u]", W[rt], 4 * sn);
        else out(L, "\tstr %s, [x15, #%u]", W[rt], 4 * sn);
        return LIFT_OK;
    }
    if ((w & 0x0fe00fd0u) == 0x0c400b10u && rt < 15 && rn < 15) { /* vmov Dm, Rt, Rt2 / vmov Rt, Rt2, Dm */
        unsigned dm = vreg_m(w);
        vbase(L);
        if (w >> 20 & 1) {
            if (rt == rn) return LIFT_NO;
            out(L, "\tldp %s, %s, [x15, #%u]", W[rt], W[rn], 8 * dm);
        } else out(L, "\tstp %s, %s, [x15, #%u]", W[rt], W[rn], 8 * dm);
        return LIFT_OK;
    }
    if ((w & 0x0fbf0ed0u) == 0x0eb00a40u || (w & 0x0fbf0ed0u) == 0x0eb00ac0u ||
        (w & 0x0fbf0ed0u) == 0x0eb10a40u) { /* vmov / vabs / vneg Vd, Vm (bit operations: no FP exceptions) */
        unsigned dbl = w >> 8 & 1, kind = (w >> 16 & 1) ? 2 : (w >> 7 & 1) ? 1 : 0;
        unsigned dd = dbl ? vreg_d(w) : ((w >> 12 & 15) << 1 | (w >> 22 & 1)), mm = dbl ? vreg_m(w) : ((w & 15) << 1 | (w >> 5 & 1));
        vbase(L);
        if (dbl) {
            out(L, "\tldr x16, [x15, #%u]", 8 * mm);
            if (kind == 1) out(L, "\tand x16, x16, #0x7fffffffffffffff");
            else if (kind == 2) out(L, "\teor x16, x16, #0x8000000000000000");
            out(L, "\tstr x16, [x15, #%u]", 8 * dd);
        } else {
            out(L, "\tldr w16, [x15, #%u]", 4 * mm);
            if (kind == 1) out(L, "\tand w16, w16, #0x7fffffff");
            else if (kind == 2) out(L, "\teor w16, w16, #0x80000000");
            out(L, "\tstr w16, [x15, #%u]", 4 * dd);
        }
        return LIFT_OK;
    }
    return LIFT_NO;
}

/* Emits the instruction. Returns LIFT_NO when the instruction has to run out of line. */
static int lift_insn(Lift *L, const A32Insn *i, unsigned cond, unsigned len, int in_it, int *handled_cond) {
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8), ret = (L->pc + len) | (L->thumb ? 1u : 0u);
    int setf = (i->fl & FL_S) || ((i->fl & FL_SIT) && !in_it);
    *handled_cond = 0;
    switch (i->op) {
        case OP_NOP: {
            /* dmb / dsb decode as NOPs in the interpreter; lifted code runs at full speed on a weakly ordered host, so the guest's barriers must stay barriers */
            uint32_t w = L->thumb ? (ld16g(L, L->pc) == 0xf3bf ? (0xf3bf0000u | ld16g(L, L->pc + 2)) : 0) : ld32g(L, L->pc);
            int bar = L->thumb ? ((w & 0xfff0) == 0x8f40 || (w & 0xfff0) == 0x8f50) && (w >> 16) == 0xf3bf : (w & 0xffffffe0u) == 0xf57ff040u;
            if (bar) out(L, "\tdmb ish");
            return LIFT_OK;
        }
        case OP_DP: return lift_dp(L, i, setf);
        case OP_MOVW:
            if (i->rd == 15) return LIFT_NO;
            out(L, "\tmovz %s, #0x%x", W[i->rd], i->imm & 0xffff);
            return LIFT_OK;
        case OP_MOVT:
            if (i->rd == 15) return LIFT_NO;
            out(L, "\tmovk %s, #0x%x, lsl #16", W[i->rd], i->imm & 0xffff);
            return LIFT_OK;
        case OP_MUL:
        case OP_MULL: return lift_mul(L, i, setf);
        case OP_DIV:
        case OP_CLZ:
        case OP_RBIT:
        case OP_REV:
        case OP_REV16:
        case OP_REVSH:
        case OP_EXT:
        case OP_BFX:
        case OP_BFI: return lift_misc(L, i);
        case OP_SMULH:
        case OP_SMMUL: return lift_smul(L, i);
        case OP_LDST: return lift_ldst(L, i);
        case OP_LDM: return lift_ldm(L, i);
        case OP_LDREX:
        case OP_STREX: return lift_excl(L, i);
        case OP_CLREX: out(L, "\tclrex"); return LIFT_OK;
        case OP_B:
        case OP_CBZ: return lift_branch(L, i, cond, len, handled_cond);
        case OP_BL:
            movc(L, "w14", ret);
            jump_to(L, pcv + i->imm, L->thumb, 14);
            return LIFT_TERM;
        case OP_BLX_IMM: {
            uint32_t base = L->thumb ? (pcv & ~3u) : pcv;
            movc(L, "w14", ret);
            jump_to(L, base + i->imm, !L->thumb, 14);
            return LIFT_TERM;
        }
        case OP_BX: {
            if (i->rm == 15) {
                movc(L, "w24", pcv);
                jump_ind(L, "w24", 0);
                return LIFT_TERM;
            }
            jump_ind(L, W[i->rm], 0);
            return LIFT_TERM;
        }
        case OP_BLX_REG: {
            if (i->rm == 15) return LIFT_NO;
            out(L, "\tmov w24, %s", W[i->rm]);
            movc(L, "w14", ret);
            jump_ind(L, "w24", 0);
            return LIFT_TERM;
        }
        case OP_TB: {
            const char *b = "w15";
            if (i->rn == 15) movc(L, "w15", pcv);
            else b = W[i->rn];
            if (i->rm == 15) return LIFT_NO;
            if (i->sub == 0) {
                out(L, "\tadd w16, %s, %s", b, W[i->rm]);
                out(L, "\tldrb w16, [x20, w16, uxtw]");
            } else {
                out(L, "\tadd w16, %s, %s, lsl #1", b, W[i->rm]);
                out(L, "\tldrh w16, [x20, w16, uxtw]");
            }
            movc(L, "w15", pcv);
            out(L, "\tadd w24, w15, w16, lsl #1");
            jump_ind(L, "w24", 1);
            return LIFT_TERM;
        }
        case OP_VFP: return lift_vfp(L, i);
        case OP_NEON: return lift_neon(L, i);
        case OP_MRC:
            if ((i->imm & 15) == 15 && ((i->imm >> 16) & 7) == 0 && ((i->imm >> 12) & 15) == 13 && ((i->imm >> 8) & 15) == 0 && ((i->imm >> 4) & 7) == 3 &&
                i->rd != 15) {
                out(L, "\tldr %s, [x21, #%zu]", W[i->rd], offsetof(A32Cpu, tpidruro));
                return LIFT_OK;
            }
            return LIFT_NO;
        default: return LIFT_NO;
    }
}


static const char *const OPNAMES[] = {"undef", "unimpl", "dp",    "movw",  "movt",  "mul",   "mull", "smulh", "smmul",   "smuad", "div",     "clz",
                                      "rbit",  "rev",    "rev16", "revsh", "sel",   "usad8", "ext",  "bfx",   "bfi",     "sat",   "pkh",     "qarith",
                                      "par",   "ldst",   "ldm",   "ldrex", "strex", "clrex", "b",    "bl",    "blx_imm", "bx",    "blx_reg", "cbz",
                                      "tb",    "it",     "svc",   "bkpt",  "nop",   "mrs",   "msr",  "mrc",   "mcr",     "mrrc",  "vfp",     "neon"};
const char *a32lift_opname(unsigned op) { return op < sizeof OPNAMES / sizeof *OPNAMES ? OPNAMES[op] : "?"; }

static void emit_fallback(Lift *L, const A32Insn *i, unsigned len, unsigned it) {
    uint32_t raw = L->thumb ? (ld16g(L, L->pc) | (len == 4 ? ld16g(L, L->pc + 2) << 16 : 0)) : ld32g(L, L->pc);
    /* the instruction was decoded here, at build time: the image carries it as data and the runtime executes its semantics (a32_op_exec) without
     * fetching or decoding any guest code */
    if (!L->pool) L->pool = open_memstream(&L->poolbuf, &L->poolsz);
    unsigned lab = L->label++;
    A32Lit lit;
    memset(&lit, 0, sizeof lit);
    lit.insn = *i;
    lit.raw = raw;
    fprintf(L->pool, "Lp%u:\n", lab);
    for (unsigned k = 0; k < sizeof lit / 4; k++) {
        uint32_t w;
        memcpy(&w, (const char *)&lit + 4 * k, 4);
        fprintf(L->pool, "\t.long 0x%x\n", w);
    }
    out(L, "\tadrp x28, Lp%u@PAGE", lab);
    out(L, "\tadd x28, x28, Lp%u@PAGEOFF", lab);
    movc(L, "w24", L->pc | (L->thumb ? 1u : 0u));
    out(L, "\tmovz w25, #%u", len);
    out(L, "\tbl _a32jit_fb");
    L->st->fallback++;
    if (L->in->seed_fb) L->in->seed_fb[(L->pc - L->in->fb_base) / L->in->fb_stride] = 1;
    if (i->op < 64) L->st->fb_by_op[i->op]++;
    if (L->in->cov) {
        const char *mn = "?";
        for (unsigned m = 0; m < L->in->nmods; m++)
            if (L->pc >= L->in->mods[m].base && L->pc - L->in->mods[m].base < L->in->mods[m].text_end) mn = L->in->mods[m].name;
        fprintf(L->in->cov, "%s %08x %c %08x %s\n", mn, L->pc, L->thumb ? 'T' : 'A', raw, a32lift_opname(i->op));
    }
    (void)it;
}

static void flush_pool(Lift *L) {
    if (!L->pool) return;
    fclose(L->pool);
    L->pool = NULL;
    fputs("\t.section __TEXT,__const\n\t.p2align 2\n", L->o);
    fwrite(L->poolbuf, 1, L->poolsz, L->o);
    fputs("\t.text\n\t.p2align 2\n", L->o);
    free(L->poolbuf);
    L->poolbuf = NULL;
}

/* ---- regions -------------------------------------------------------------------------------------------------------- */
static void switch_chunk(Lift *L, uint32_t chunk) {
    if (L->o && chunk == L->cur_chunk) return;
    if (L->o) {
        fclose(L->o);
        L->o = NULL;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s.%03x.S", L->prefix, chunk);
    L->o = fopen(path, "w");
    if (!L->o) {
        perror(path);
        exit(1);
    }
    if (L->nfiles == L->capfiles) {
        L->capfiles = L->capfiles ? L->capfiles * 2 : 64;
        L->files = realloc(L->files, L->capfiles * sizeof *L->files);
    }
    L->files[L->nfiles++] = strdup(path);
    L->cur_chunk = chunk;
    fputs("\t.text\n\t.p2align 2\n", L->o);
}

static unsigned region_len(Lift *L, uint32_t start, int thumb) {
    unsigned n = 0, it = 0;
    uint32_t addr = start;
    for (;;) {
        A32Insn i;
        unsigned len = decode_at(L, addr, thumb, &i);
        if (!len) break;
        n++;
        if (L->in->each_entry) break;
        unsigned cond = insn_cond(&i, thumb, it);
        int term = 0;
        switch (i.op) {
            case OP_B: term = cond == 15; break;
            case OP_BL:
            case OP_BLX_IMM:
            case OP_BLX_REG:
            case OP_BX:
            case OP_TB: term = cond == 15; break;
            case OP_DP: term = cond == 15 && i.rd == 15 && !(i.sub >= 8 && i.sub <= 11); break;
            case OP_LDST: term = cond == 15 && i.rd == 15 && i.sub == 1; break;
            case OP_LDM: term = cond == 15 && i.sub && (i.imm >> 15 & 1); break;
            case OP_UNDEF:
            case OP_UNIMPL: term = 1; break;
            default: break;
        }
        if (i.op == OP_IT) it = i.imm;
        else if (thumb && it) it = it_advance(it);
        if (term) break;
        addr += len;
        uint8_t *f = flagref(L, addr, thumb);
        if (!f || (*f & F_ENT)) break;
    }
    return n ? n : 1;
}

static void emit_region(Lift *L, uint32_t start, int thumb, uint32_t nextkey) {
    char nm[32];
    L->pc = start;
    L->thumb = thumb;
    lname(nm, sizeof nm, start, thumb, 0);
    out(L, "\t.globl %s\n\t.private_extern %s\n\t.p2align 2\n%s:", nm, nm, nm);
    lname(nm, sizeof nm, start, thumb, 1);
    out(L, "%s:", nm);
    L->st->entries++;
    uint8_t *f0 = flagref(L, start, thumb);
    if (*f0 & F_CHK) {
        unsigned lab = L->label++;
        out(L, "\tsub x19, x19, #%u", region_len(L, start, thumb));
        out(L, "\ttbz x19, #63, L%u", lab);
        movc(L, "w24", start | (thumb ? 1u : 0u));
        out(L, "\tmovz w25, #0");
        out(L, "\tb _a32jit_leave");
        out(L, "L%u:", lab);
    }
    unsigned it = 0;
    uint32_t addr = start;
    L->fprun = 0;
    for (;;) {
        A32Insn i;
        unsigned len = decode_at(L, addr, thumb, &i);
        L->pc = addr;
        if (!len) {
            close_run(L);
            movc(L, "w24", addr | (thumb ? 1u : 0u));
            out(L, "\tb _a32jit_noblock");
            return;
        }
        unsigned cond = insn_cond(&i, thumb, it);
        int in_it = thumb && it != 0;
        L->st->lifted++;
        if (i.op == OP_IT) {
            it = i.imm;
            goto next;
        }
        {
            /* conditions: lift under a skip branch (except branches, which fold the condition into the jump) */
            unsigned lab = 0;
            int skipped = 0;
            int is_branch = i.op == OP_B || i.op == OP_CBZ;
            if (run_for_flow(&i) || (cond < 14 && !is_branch)) close_run(L); /* a floating point run never crosses a jump or a skipped instruction */
            if (cond < 14 && !is_branch) {
                lab = L->label++;
                out(L, "\tb.%s L%u", CC[cond ^ 1], lab);
                skipped = 1;
            }
            int handled = 0;
            /* a probe: lift into a scratch buffer so a refusal leaves no partial output */
            FILE *real = L->o;
            char *buf = NULL;
            size_t bsz = 0;
            L->o = open_memstream(&buf, &bsz);
            int r = lift_insn(L, &i, cond, len, in_it, &handled);
            fclose(L->o);
            L->o = real;
            if (r != LIFT_NO) {
                fwrite(buf, 1, bsz, L->o);
                free(buf);
            } else {
                free(buf);
                L->st->lifted--;
                close_run(L); /* the out-of-line operation runs host code */
                emit_fallback(L, &i, len, it);
                r = LIFT_OK;
            }
            if (skipped) close_run(L);
            if (skipped) out(L, "L%u:", lab);
            if (r == LIFT_TERM && cond == 15) return;
        }
    next:
        if (L->in->each_entry) {
            close_run(L);
            addr += len;
            movc(L, "w24", addr | (thumb ? 1u : 0u));
            out(L, "\tb _a32jit_noblock");
            return;
        }
        if (i.op != OP_IT && thumb && it) it = it_advance(it);
        addr += len;
        uint8_t *f = flagref(L, addr, thumb);
        if (!f) {
            close_run(L);
            movc(L, "w24", addr | (thumb ? 1u : 0u));
            out(L, "\tb _a32jit_noblock");
            return;
        }
        if (*f & F_ENT) {
            close_run(L);
            L->pc = addr;
            if ((addr | (thumb ? 1u : 0u)) != nextkey) jump_to(L, addr, thumb, 14);
            return;
        }
    }
}

/* ---- hash table and stubs ------------------------------------------------------------------------------------------ */
typedef struct {
    uint32_t key;
} HEnt;

static const char *const STUBS = "\t.macro A32_SPILL\n"
                                 "\tstp w0, w1, [x21, #0]\n\tstp w2, w3, [x21, #8]\n\tstp w4, w5, [x21, #16]\n\tstp w6, w7, [x21, #24]\n"
                                 "\tstp w8, w9, [x21, #32]\n\tstp w10, w11, [x21, #40]\n\tstp w12, w13, [x21, #48]\n\tstr w14, [x21, #56]\n"
                                 "\tmrs x16, nzcv\n\tlsr w15, w16, #31\n\tubfx w17, w16, #30, #1\n\tstp w15, w17, [x21, #%zu]\n"
                                 "\tubfx w15, w16, #29, #1\n\tubfx w17, w16, #28, #1\n\tstp w15, w17, [x21, #%zu]\n"
                                 "\t.endm\n"
                                 "\t.macro A32_RELOAD\n"
                                 "\tldp w0, w1, [x21, #0]\n\tldp w2, w3, [x21, #8]\n\tldp w4, w5, [x21, #16]\n\tldp w6, w7, [x21, #24]\n"
                                 "\tldp w8, w9, [x21, #32]\n\tldp w10, w11, [x21, #40]\n\tldp w12, w13, [x21, #48]\n\tldr w14, [x21, #56]\n"
                                 "\tldp w16, w17, [x21, #%zu]\n\tlsl w16, w16, #31\n\torr w16, w16, w17, lsl #30\n"
                                 "\tldp w15, w17, [x21, #%zu]\n\torr w16, w16, w15, lsl #29\n\torr w16, w16, w17, lsl #28\n\tmsr nzcv, x16\n"
                                 "\t.endm\n";

static void emit_tab(Lift *L, const char *path, const uint32_t *keys, unsigned nkeys) {
    FILE *o = fopen(path, "w");
    if (!o) {
        perror(path);
        exit(1);
    }
    L->o = o;
    unsigned bits = 4;
    while ((1u << bits) < nkeys * 2) bits++;
    unsigned size = 1u << bits;
    uint32_t *slot = calloc(size, sizeof *slot);
    uint8_t *used = calloc(size, 1);
    for (unsigned k = 0; k < nkeys; k++) {
        unsigned h = (uint32_t)(keys[k] * 0x9E3779B1u) >> (32 - bits);
        while (used[h]) h = (h + 1) & (size - 1);
        used[h] = 1;
        slot[h] = keys[k];
    }
    size_t n_off = offsetof(A32Cpu, n), c_off = offsetof(A32Cpu, c);
    fprintf(o, "\t.text\n\t.p2align 2\n");
    fprintf(o, STUBS, n_off, c_off, n_off, c_off);
    fprintf(o, "\t.globl _a32jit_hash_bits\n\t.globl _a32jit_hash\n\t.globl _a32jit_hooks\n\t.globl _a32jit_enter\n\t.globl _a32jit_ind\n");
    fprintf(
        o,
        "\t.p2align 2\n"
        /* int a32jit_enter(A32Cpu *c, int64_t budget): runs lifted code from c->r[15] until it leaves; returns the A32Exit or EXIT_NOBLOCK */
        "_a32jit_enter:\n\tstp x29, x30, [sp, #-96]!\n\tstp x19, x20, [sp, #16]\n\tstp x21, x22, [sp, #32]\n\tstp x23, x24, [sp, #48]\n\tstp x25, x26, [sp, #64]\n\tstp x27, x28, [sp, #80]\n"
        "\tmov x21, x0\n\tmov x19, x1\n\tldr x20, [x21, #%zu]\n\tadrp x22, _a32jit_hash@PAGE\n\tadd x22, x22, _a32jit_hash@PAGEOFF\n"
        "\tA32_RELOAD\n\tldr w24, [x21, #%zu]\n\tldr w16, [x21, #%zu]\n\torr w24, w24, w16\n\tb _a32jit_ind\n",
        offsetof(A32Cpu, mem), offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, thumb));
    fprintf(
        o,
        /* w24 = guest address | thumb: look the block up */
        "_a32jit_ind:\n\tmovz w26, #0x79b1\n\tmovk w26, #0x9e37, lsl #16\n\tmul w25, w24, w26\n\tlsr w25, w25, #%u\n"
        "1:\tadd x26, x22, x25, lsl #4\n\tldp x27, x28, [x26]\n\tcbz x28, 2f\n\teor w27, w27, w24\n\tcbz w27, 3f\n"
        "\tadd w25, w25, #1\n\tand w25, w25, #0x%x\n\tb 1b\n3:\tbr x28\n"
        "2:\tlsr w26, w24, #20\n\teor w26, w26, #0xf00\n\tcbz w26, _a32jit_hle\n"
        "\t.globl _a32jit_noblock\n_a32jit_noblock:\n\tmovz w25, #%d\n"
        /* w24 = next pc | thumb, w25 = reason: save the guest state and return */
        "\t.globl _a32jit_leave\n_a32jit_leave:\n\tA32_SPILL\n\tand w15, w24, #1\n\tstr w15, [x21, #%zu]\n\tand w15, w24, #0xfffffffe\n\tstr w15, [x21, #%zu]\n\tstr wzr, [x21, #%zu]\n"
        "_a32jit_ret:\n\tmov w0, w25\n\tldp x19, x20, [sp, #16]\n\tldp x21, x22, [sp, #32]\n\tldp x23, x24, [sp, #48]\n\tldp x25, x26, [sp, #64]\n\tldp x27, x28, [sp, #80]\n\tldp x29, x30, [sp], #96\n\tret\n",
        32 - bits, size - 1, EXIT_NOBLOCK, offsetof(A32Cpu, thumb), offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, itstate));
    fprintf(o,
            /* out-of-line operation: x28 = the build-time decoded instruction, w24 = its pc | thumb, w25 = its length. The host function a32jit_op
             * executes the semantics; if the pc it leaves differs from the next instruction the lifted code continues through the block table. */
            "\t.globl _a32jit_fb\n_a32jit_fb:\n\tmov x27, x30\n\tA32_SPILL\n"
            "\tand w15, w24, #1\n\tstr w15, [x21, #%zu]\n\tand w16, w24, #0xfffffffe\n\tstr w16, [x21, #%zu]\n\tstr wzr, [x21, #%zu]\n"
            "\tadd w26, w16, w25\n\torr w26, w26, w15\n"
            "\tadrp x16, _a32jit_hooks@PAGE\n\tldr x16, [x16, _a32jit_hooks@PAGEOFF]\n\tmov x0, x21\n\tmov x1, x28\n\tblr x16\n"
            "\tcbnz w0, 4f\n\tA32_RELOAD\n"
            "\tldr w16, [x21, #%zu]\n\tldr w17, [x21, #%zu]\n\torr w24, w16, w17\n\teor w16, w24, w26\n\tcbnz w16, _a32jit_ind\n\tret x27\n"
            "4:\tmov w25, w0\n\tb _a32jit_ret\n",
            offsetof(A32Cpu, thumb), offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, itstate), offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, thumb));
    fprintf(o,
            /* a branch into the host-implemented import stubs (guest address 0xF00xxxxx): call the host function right here and continue at the guest's lr,
         * without leaving the lifted code */
            "_a32jit_hle:\n\tA32_SPILL\n\tmov x0, x21\n\tmov w1, w24\n\tadrp x16, _a32jit_hooks@PAGE\n\tldr x16, [x16, _a32jit_hooks@PAGEOFF + 8]\n\tblr x16\n"
            "\tcbnz w0, 6f\n\tA32_RELOAD\n\tldr w24, [x21, #%zu]\n\tldr w16, [x21, #%zu]\n\torr w24, w24, w16\n\tb _a32jit_ind\n"
            "6:\tmovz w25, #1\n\tb _a32jit_ret\n",
            offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, thumb));
    fprintf(o, "\t.data\n\t.p2align 4\n_a32jit_hooks:\n\t.quad 0, 0\n\t.p2align 4\n_a32jit_hash_bits:\n\t.quad %u\n", bits);
    fprintf(o, "\t.section __DATA,__const\n\t.p2align 4\n_a32jit_hash:\n");
    for (unsigned h = 0; h < size; h++) {
        if (!used[h]) {
            fprintf(o, "\t.quad 0, 0\n");
            continue;
        }
        char nm[32];
        lname(nm, sizeof nm, slot[h] & ~1u, slot[h] & 1, 0);
        fprintf(o, "\t.quad 0x%x, %s\n", slot[h], nm);
    }
    fclose(o);
    free(slot);
    free(used);
}

int a32lift_run(const LiftInput *in, const char *prefix, char ***out_files, LiftStats *st) {
    Lift L;
    memset(&L, 0, sizeof L);
    L.in = in;
    L.prefix = prefix;
    L.st = st;
    memset(st, 0, sizeof *st);
    L.mf = calloc(in->nmods, sizeof *L.mf);
    for (unsigned m = 0; m < in->nmods; m++) {
        L.mf[m].base = in->mods[m].base;
        L.mf[m].text_end = in->mods[m].text_end;
        L.mf[m].fa = calloc(in->mods[m].text_end / 4 + 2, 1);
        L.mf[m].ft = calloc(in->mods[m].text_end / 2 + 2, 1);
    }
    if (in->each_entry) {
        for (unsigned s = 0; s < in->nseeds; s++) {
            uint8_t *f = flagref(&L, in->seeds[s] & ~1u, in->seeds[s] & 1);
            if (f) *f |= F_ENT | F_CHK | F_VIS;
        }
    } else discover(&L);
    if (getenv("A32LIFT_WHY")) { /* debugging: the discovery flags of a guest address */
        uint32_t a = (uint32_t)strtoul(getenv("A32LIFT_WHY"), NULL, 16);
        uint8_t *fa = flagref(&L, a, 0), *ft = flagref(&L, a, 1);
        fprintf(stderr, "a32lift: %#x arm flags %d thumb flags %d (1 entry, 2 check, 4 visited)\n", a, fa ? *fa : -1, ft ? *ft : -1);
    }
    /* collect entries in address order */
    uint32_t *keys = NULL;
    unsigned nkeys = 0, cap = 0;
    for (unsigned m = 0; m < in->nmods; m++) {
        for (uint32_t off = 0; off < in->mods[m].text_end; off += 2) {
            for (int th = 0; th < 2; th++) {
                if (!th && (off & 2)) continue;
                uint8_t f = th ? L.mf[m].ft[off >> 1] : L.mf[m].fa[off >> 2];
                if (!(f & F_ENT)) continue;
                if (nkeys == cap) {
                    cap = cap ? cap * 2 : 1024;
                    keys = realloc(keys, cap * sizeof *keys);
                }
                keys[nkeys++] = (in->mods[m].base + off) | (uint32_t)th;
            }
        }
    }
    for (unsigned k = 0; k < nkeys; k++) {
        uint32_t a = keys[k] & ~1u;
        int th = keys[k] & 1;
        switch_chunk(&L, chunk_of(a));
        /* the next key is the physical successor in this file: a region that is cut there falls through into it */
        uint32_t nk = k + 1 < nkeys && chunk_of(keys[k + 1] & ~1u) == chunk_of(a) ? keys[k + 1] : 0xffffffffu;
        L.pc = a;
        L.thumb = th;
        emit_region(&L, a, th, nk);
        flush_pool(&L);
    }
    if (L.o) fclose(L.o);
    char tab[1024];
    snprintf(tab, sizeof tab, "%s.tab.S", prefix);
    emit_tab(&L, tab, keys, nkeys);
    if (L.nfiles == L.capfiles) {
        L.capfiles++;
        L.files = realloc(L.files, L.capfiles * sizeof *L.files);
    }
    L.files[L.nfiles++] = strdup(tab);
    *out_files = L.files;
    for (unsigned m = 0; m < in->nmods; m++) {
        free(L.mf[m].fa);
        free(L.mf[m].ft);
    }
    free(L.mf);
    free(L.stack);
    free(keys);
    return (int)L.nfiles;
}
