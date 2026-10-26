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

typedef struct { uint8_t *fa, *ft; uint32_t base, text_end; } ModFlags;

typedef struct {
    const LiftInput *in;
    ModFlags *mf;
    uint32_t *stack; size_t nstack, capstack;
    FILE *o; unsigned label;
    uint32_t cur_chunk;
    LiftStats *st;
    const char *prefix; char **files; unsigned nfiles, capfiles;
    /* per-instruction emission context */
    uint32_t pc; int thumb;
} Lift;

static const char *const W[16] = {"w0","w1","w2","w3","w4","w5","w6","w7","w8","w9","w10","w11","w12","w13","w14","w15"};
static const char *const X[16] = {"x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15"};
static const char *const CC[16] = {"eq","ne","hs","lo","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al","al"};

static void out(Lift *L, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(Lift *L, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(L->o, fmt, ap); fputc('\n', L->o);
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
static uint32_t ld32g(const Lift *L, uint32_t a) { uint32_t v; memcpy(&v, L->in->mem + a, 4); return v; }
static uint32_t ld16g(const Lift *L, uint32_t a) { uint16_t v; memcpy(&v, L->in->mem + a, 2); return v; }

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
    if (L->nstack == L->capstack) { L->capstack = L->capstack ? L->capstack * 2 : 4096; L->stack = realloc(L->stack, L->capstack * sizeof *L->stack); }
    L->stack[L->nstack++] = addr | (thumb ? 1u : 0u);
}
static void add_entry(Lift *L, uint32_t addr, int thumb, int chk) {
    uint8_t *f = flagref(L, addr, thumb);
    if (!f) return;
    *f |= F_ENT | (chk ? F_CHK : 0);
    if (!(*f & F_VIS)) push(L, addr, thumb);
}

typedef struct { uint32_t addr; A32Insn i; } Recent;

static void discover_tb(Lift *L, const A32Insn *i, uint32_t pcv, const Recent *rec, unsigned nrec) {
    unsigned n = 0;
    for (unsigned k = 0; k < nrec; k++) {   /* cmp rX, #N shortly before: N + 1 table entries */
        const A32Insn *p = &rec[nrec - 1 - k].i;
        if (p->op == OP_DP && p->sub == 10 && p->k == K_IMM && p->rn == i->rm && p->imm < 512) { n = p->imm + 1; break; }
    }
    if (!n) n = 16;
    for (unsigned e = 0; e < n; e++) {
        uint32_t off = i->sub == 0 ? (uint32_t)L->in->mem[pcv + e] : ld16g(L, pcv + 2 * e);
        uint32_t t = pcv + off * 2;
        if (flagref(L, t, 1)) add_entry(L, t, 1, 1);
    }
}

/* push {..., lr} (ARM stmdb sp!, Thumb push / stmdb sp!) or the common `mov ip, sp` / `sub sp` openers: the first instruction of a function. */
static int looks_like_prologue(Lift *L, uint32_t a, int thumb) {
    if (!flagref(L, a, thumb)) return 0;
    if (thumb) {
        uint32_t h = ld16g(L, a);
        if ((h & 0xff00) == 0xb500) return 1;                               /* push {..., lr} */
        if (h == 0xe92d && (ld16g(L, a + 2) & 0x4000)) return 1;           /* stmdb sp!, {..., lr} */
        return 0;
    }
    if (a & 3) return 0;
    uint32_t w = ld32g(L, a);
    return (w & 0xffff4000u) == 0xe92d4000u || w == 0xe1a0c00du || (w & 0xfffff000u) == 0xe24dd000u || w == 0xe52de004u;   /* push {..lr}; mov ip, sp; sub sp, sp, #n; str lr, [sp, #-4]! */
}

static void walk_run(Lift *L, uint32_t start, int thumb) {
    unsigned it = 0;
    Recent rec[8]; unsigned nrec = 0;
    uint32_t addr = start;
    for (;;) {
        uint8_t *f = flagref(L, addr, thumb);
        if (!f) break;
        if (*f & F_VIS) { if (addr != start) *f |= F_ENT; break; }
        *f |= F_VIS;
        A32Insn i; unsigned len = decode_at(L, addr, thumb, &i);
        if (!len) break;
        L->st->insns++;
        uint32_t pcv = addr + (thumb ? 4 : 8);
        unsigned cond = insn_cond(&i, thumb, it);
        int uncond = cond == 15, stop = 0;
        switch (i.op) {
        case OP_B: { uint32_t t = pcv + i.imm; add_entry(L, t, thumb, t <= addr); stop = uncond; break; }
        case OP_BL: case OP_BLX_IMM: {
            uint32_t base = i.op == OP_BLX_IMM && thumb ? (pcv & ~3u) : pcv, t = base + i.imm;
            add_entry(L, t, i.op == OP_BLX_IMM ? !thumb : thumb, 1);
            add_entry(L, addr + len, thumb, 1);
            stop = uncond; break;
        }
        case OP_BLX_REG: add_entry(L, addr + len, thumb, 1); stop = uncond; break;
        case OP_BX: stop = uncond; break;
        case OP_CBZ: { uint32_t t = pcv + i.imm; add_entry(L, t, thumb, t <= addr); break; }
        case OP_TB: discover_tb(L, &i, pcv, rec, nrec); stop = uncond; break;
        case OP_DP:
            if (i.rd == 15 && !(i.sub >= 8 && i.sub <= 11)) {
                stop = uncond;
                if (!thumb && i.sub == 4 && i.rn == 15) {   /* add pc, pc, rX, lsl #2 followed by a table of branches: every one is an entry */
                    for (uint32_t t = addr + 4; ; t += 4) {
                        A32Insn bi; unsigned bl = decode_at(L, t, 0, &bi);
                        if (!bl || bi.op != OP_B || bi.cond != 15) break;
                        add_entry(L, t, 0, 1);
                    }
                }
            }
            break;
        case OP_LDST: if (i.rd == 15 && i.sub == 1) stop = uncond; break;
        case OP_LDM: if (i.sub && (i.imm >> 15 & 1)) stop = uncond; break;
        case OP_UNDEF: case OP_UNIMPL: stop = 1; break;
        default: break;
        }
        if (nrec == 8) { memmove(rec, rec + 1, 7 * sizeof *rec); nrec--; }
        rec[nrec].addr = addr; rec[nrec].i = i; nrec++;
        if (i.op == OP_IT) it = i.imm; else if (thumb && it) it = it_advance(it);
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
                       (pp == 0xe8bd && (p & 0x8000)) || (pp == 0xf85d && p == 0xfb04);   /* pop.w {.., pc}; ldr pc, [sp], #4 */
            if (ends) add_entry(L, a, 1, 1);
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
}

/* ---- emission helpers ---------------------------------------------------------------------------------------------- */
static void movc(Lift *L, const char *w, uint32_t v) {
    if (v <= 0xffff) out(L, "\tmovz %s, #0x%x", w, v);
    else if (~v <= 0xffff) out(L, "\tmovn %s, #0x%x", w, ~v & 0xffff);
    else if (!(v & 0xffff)) out(L, "\tmovz %s, #0x%x, lsl #16", w, v >> 16);
    else { out(L, "\tmovz %s, #0x%x", w, v & 0xffff); out(L, "\tmovk %s, #0x%x, lsl #16", w, v >> 16); }
}
static uint32_t chunk_of(uint32_t a) { return a >> CHUNK_SHIFT; }
static void lname(char *b, size_t n, uint32_t a, int thumb, int local) { snprintf(b, n, "%s%c_%08x", local ? "L" : "", thumb ? 'T' : 'A', a); }

/* Jump to a guest address known at lift time. `cond` 0..13 = only when that A64 condition holds. */
static void jump_to(Lift *L, uint32_t t, int tthumb, int cond) {
    uint8_t *f = flagref(L, t, tthumb);
    char nm[32];
    if (!f || !(*f & F_ENT)) {   /* nothing lifted there: leave to the dispatcher (which interprets) */
        unsigned lab = L->label++;
        if (cond < 14) out(L, "\tb.%s L%u", CC[cond ^ 1], lab);
        movc(L, "w24", t | tthumb);
        out(L, "\tb _a32jit_noblock");
        if (cond < 14) out(L, "L%u:", lab);
        return;
    }
    int same = L->cur_chunk == chunk_of(t);
    lname(nm, sizeof nm, t, tthumb, same);
    if (cond >= 14) { out(L, "\tb %s", nm); return; }
    if (same) { out(L, "\tb.%s %s", CC[cond], nm); return; }
    unsigned lab = L->label++;
    out(L, "\tb.%s L%u", CC[cond ^ 1], lab); out(L, "\tb %s", nm); out(L, "L%u:", lab);
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
    int is_imm; uint32_t imm;
    int carry;          /* 0 unchanged, 1 constant bit in cbit, 2 bit `cbit` of register `csrc` (an x register name) */
    int cbit; char csrc[8];
} Op2;

/* Builds operand 2 of a data-processing instruction. logical: the A64 form accepts a ror shifted operand; plain: must be a register (no shift
 * left in the instruction text). The register-shifted forms and every materialisation go through w17 (and w26..w28). Returns 0 if not supported. */
static int build_op2(Lift *L, const A32Insn *i, uint32_t pcv, int logical, int plain, Op2 *o) {
    memset(o, 0, sizeof *o);
    if (i->k == K_IMM) {
        o->is_imm = 1; o->imm = i->imm;
        if (i->fl & FL_ICARRY) { o->carry = 1; o->cbit = i->imm >> 31; }
        if (plain) { movc(L, "w16", i->imm); strcpy(o->text, "w16"); }
        return 1;
    }
    const char *src; char xs[8];
    if (i->rm == 15) { movc(L, "w26", pcv); src = "w26"; strcpy(xs, "x26"); } else { src = W[i->rm]; snprintf(xs, sizeof xs, "x%u", i->rm); }
    if (i->k == K_RSR) {
        const char *ws = W[i->ra];
        if (i->ra == 15) return 0;
        switch (i->sh) {
        case 0:
        case 1:
            out(L, "\tand w17, %s, #0xff", ws);
            out(L, "\t%s x28, %s, x17", i->sh ? "lsr" : "lsl", xs);
            out(L, "\tlsr w17, w17, #6"); out(L, "\tsub w17, w17, #1"); out(L, "\tasr w17, w17, #31"); out(L, "\tand w17, w28, w17");
            break;
        case 2:
            out(L, "\tand w17, %s, #0xff", ws);
            out(L, "\tsxtw x28, %s", src);
            out(L, "\tlsr w27, w17, #6"); out(L, "\tneg w27, w27"); out(L, "\tasr w27, w27, #31"); out(L, "\torr w17, w17, w27");
            out(L, "\tasr x28, x28, x17"); out(L, "\tmov w17, w28");
            break;
        default: out(L, "\tror w17, %s, %s", src, ws); break;
        }
        strcpy(o->text, "w17");
        o->carry = -1;   /* the shifter carry is not modelled: callers that need it fall back */
        return 1;
    }
    unsigned sh = i->sh, sa = i->sa;
    o->carry = 2; snprintf(o->csrc, sizeof o->csrc, "%s", xs);
    switch (sh) {
    case 0: if (sa == 0) o->carry = 0; else o->cbit = 32 - sa; break;
    case 1: o->cbit = sa == 0 ? 31 : sa - 1; break;
    case 2: o->cbit = sa == 0 ? 31 : sa - 1; break;
    default: o->cbit = sa == 0 ? 0 : sa - 1; break;
    }
    if (sh == 3 && sa == 0) {   /* rrx */
        out(L, "\tcset w17, hs"); out(L, "\textr w17, w17, %s, #1", src); strcpy(o->text, "w17"); return 1;
    }
    if (!plain && (sh != 3 || logical) && !(sh == 1 && sa == 0)) {
        static const char *const sn[4] = {"lsl", "lsr", "asr", "ror"};
        unsigned a = (sh == 2 && sa == 0) ? 31 : sa;
        if (sa == 0 && sh == 0) strcpy(o->text, src);
        else snprintf(o->text, sizeof o->text, "%s, %s #%u", src, sn[sh], a);
        return 1;
    }
    if (sh == 1 && sa == 0) { if (plain) { out(L, "\tmovz w17, #0"); strcpy(o->text, "w17"); } else strcpy(o->text, "wzr"); return 1; }
    if (sh == 0 && sa == 0) { strcpy(o->text, src); return 1; }
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

static int lift_dp(Lift *L, const A32Insn *i, int setf) {
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8);
    unsigned sub = i->sub, rd = i->rd;
    int wr = !(sub >= 8 && sub <= 11);
    if (sub > 16) return LIFT_NO;
    if (!wr) setf = 1;
    if (rd == 15 && wr && setf) return LIFT_NO;
    int logical = sub == 0 || sub == 1 || sub == 8 || sub == 9 || sub == 12 || sub == 13 || sub == 14 || sub == 15 || sub == 16;
    if (i->k == K_RSR && logical && setf) return LIFT_NO;
    const char *a = "wzr";
    if (sub != 13 && sub != 15) {
        if (i->rn == 15) { movc(L, "w15", i->k == K_IMM ? (pcv & ~3u) : pcv); a = "w15"; } else a = W[i->rn];
    }
    const char *d = !wr ? "wzr" : rd == 15 ? "w24" : W[rd];
    int needs_plain = !logical && (sub == 3 || sub == 5 || sub == 6 || sub == 7);
    Op2 o;
    if (logical) {
        int imm_mov = i->k == K_IMM && (sub == 13 || sub == 15);
        if (!imm_mov) {
            if (!build_op2(L, i, pcv, 1, i->k == K_IMM, &o)) return LIFT_NO;
        } else { memset(&o, 0, sizeof o); o.is_imm = 1; o.imm = i->imm; if (i->fl & FL_ICARRY) { o.carry = 1; o.cbit = i->imm >> 31; } }
        char csrc[8] = "";
        if (setf) {
            out(L, "\tmrs x28, nzcv");
            if (o.carry == 2) {
                snprintf(csrc, sizeof csrc, "%s", o.csrc);
                if (!strcmp(d, W[i->rm]) && i->k == K_RSI && i->rm < 15) { out(L, "\tmov w27, %s", W[i->rm]); strcpy(csrc, "x27"); }
            }
        }
        const char *dd = d, *B = o.text;
        int need_tst = setf;
        switch (sub) {
        case 0: out(L, "\t%s %s, %s, %s", setf ? "ands" : "and", d, a, B); need_tst = 0; break;
        case 8: out(L, "\tands wzr, %s, %s", a, B); need_tst = 0; break;
        case 1: out(L, "\teor %s, %s, %s", d, a, B); break;
        case 9: out(L, "\teor w27, %s, %s", a, B); dd = "w27"; break;
        case 12: out(L, "\torr %s, %s, %s", d, a, B); break;
        case 14: out(L, "\t%s %s, %s, %s", setf ? "bics" : "bic", d, a, B); need_tst = 0; break;
        case 16: out(L, "\torn %s, %s, %s", d, a, B); break;
        case 13: if (i->k == K_IMM) movc(L, d, i->imm); else out(L, "\torr %s, wzr, %s", d, B); break;
        default: if (i->k == K_IMM) movc(L, d, ~i->imm); else out(L, "\tmvn %s, %s", d, B); break;   /* 15 */
        }
        if (setf) {
            if (need_tst) out(L, "\ttst %s, %s", dd, dd);
            out(L, "\trmif x28, #28, #%d", o.carry == 0 ? 3 : 1);
            if (o.carry == 1) { out(L, "\tmovz x27, #%d", o.cbit); out(L, "\trmif x27, #63, #2"); }
            else if (o.carry == 2) out(L, "\trmif %s, #%u, #2", csrc, (unsigned)((o.cbit - 1) & 63));
        }
    } else {
        int imm12 = i->k == K_IMM && i->imm <= 0xfff && (sub == 2 || sub == 4 || sub == 10 || sub == 11);
        if (imm12) { memset(&o, 0, sizeof o); snprintf(o.text, sizeof o.text, "#%u", i->imm); }
        else if (!build_op2(L, i, pcv, 0, needs_plain || i->k == K_IMM, &o)) return LIFT_NO;
        const char *S = setf ? "s" : "";
        switch (sub) {
        case 2: out(L, "\tsub%s %s, %s, %s", S, d, a, o.text); break;
        case 3: out(L, "\tsub%s %s, %s, %s", S, d, o.text, a); break;
        case 4: out(L, "\tadd%s %s, %s, %s", S, d, a, o.text); break;
        case 5: out(L, "\tadc%s %s, %s, %s", S, d, a, o.text); break;
        case 6: out(L, "\tsbc%s %s, %s, %s", S, d, a, o.text); break;
        case 7: out(L, "\tsbc%s %s, %s, %s", S, d, o.text, a); break;
        case 10: out(L, "\tcmp %s, %s", a, o.text); break;
        default: out(L, "\tcmn %s, %s", a, o.text); break;   /* 11 */
        }
    }
    if (rd == 15 && wr) { jump_ind(L, "w24", L->thumb ? 1 : 0); return LIFT_TERM; }
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
    case OP_DIV: if (i->rn == 15 || i->rm == 15) return LIFT_NO; out(L, "\t%s %s, %s, %s", i->sub ? "udiv" : "sdiv", W[i->rd], W[i->rn], W[i->rm]); return LIFT_OK;
    case OP_CLZ: if (i->rm == 15) return LIFT_NO; out(L, "\tclz %s, %s", W[i->rd], W[i->rm]); return LIFT_OK;
    case OP_RBIT: if (i->rm == 15) return LIFT_NO; out(L, "\trbit %s, %s", W[i->rd], W[i->rm]); return LIFT_OK;
    case OP_REV: if (i->rm == 15) return LIFT_NO; out(L, "\trev %s, %s", W[i->rd], W[i->rm]); return LIFT_OK;
    case OP_REV16: if (i->rm == 15) return LIFT_NO; out(L, "\trev16 %s, %s", W[i->rd], W[i->rm]); return LIFT_OK;
    case OP_REVSH: if (i->rm == 15) return LIFT_NO; out(L, "\trev16 w16, %s", W[i->rm]); out(L, "\tsxth %s, w16", W[i->rd]); return LIFT_OK;
    case OP_EXT: {
        if (i->sub > 3 || i->rm == 15) return LIFT_NO;
        static const char *const ext[4] = {"sxtb", "sxth", "uxtb", "uxth"};
        const char *src = W[i->rm];
        if (i->sa) { out(L, "\tror w16, %s, #%u", src, i->sa); src = "w16"; }
        if (i->rn == 15) out(L, "\t%s %s, %s", ext[i->sub], W[i->rd], src);
        else out(L, "\tadd %s, %s, %s, %s", W[i->rd], W[i->rn], src, ext[i->sub]);
        return LIFT_OK;
    }
    case OP_BFX:
        if (i->rn == 15 || i->imm2 == 0 || i->imm + i->imm2 > 32) return LIFT_NO;
        out(L, "\t%s %s, %s, #%u, #%u", i->sub ? "sbfx" : "ubfx", W[i->rd], W[i->rn], i->imm, i->imm2); return LIFT_OK;
    case OP_BFI:
        if (i->imm2 == 0 || i->imm + i->imm2 > 32) return LIFT_NO;
        out(L, "\tbfi %s, %s, #%u, #%u", W[i->rd], i->rn == 15 ? "wzr" : W[i->rn], i->imm, i->imm2); return LIFT_OK;
    default: return LIFT_NO;
    }
}

static int lift_smul(Lift *L, const A32Insn *i) {
    if (i->rd == 15 || i->rn == 15 || i->rm == 15) return LIFT_NO;
    if (i->op == OP_SMULH && i->sub == 3) {   /* smul<x><y> */
        out(L, "\tsbfx w16, %s, #%u, #16", W[i->rn], (i->imm & 1) ? 16 : 0);
        out(L, "\tsbfx w17, %s, #%u, #16", W[i->rm], (i->imm & 2) ? 16 : 0);
        out(L, "\tmul %s, w16, w17", W[i->rd]);
        return LIFT_OK;
    }
    if (i->op == OP_SMMUL && i->sub == 0 && i->ra == 15 && !i->imm) {
        out(L, "\tsmull x16, %s, %s", W[i->rn], W[i->rm]); out(L, "\tlsr x16, x16, #32"); out(L, "\tmov %s, w16", W[i->rd]);
        return LIFT_OK;
    }
    return LIFT_NO;
}

static int lift_ldst(Lift *L, const A32Insn *i) {
    uint32_t pcv = L->pc + (L->thumb ? 4 : 8);
    unsigned sub = i->sub, rn = i->rn, rd = i->rd;
    int wb = (i->fl & FL_W) != 0, pre = (i->fl & FL_P) != 0, up = (i->fl & FL_U) != 0;
    if (i->fl & FL_ACQ) return LIFT_NO;
    int dbl = sub >= 8, load = dbl ? sub == 9 : (sub & 1 || sub >= 6);
    if (rn == 15 && wb) return LIFT_NO;
    if (dbl && (rd == 15 || i->ra == 15 || rd == i->ra)) return LIFT_NO;
    if (!load && !dbl && rd == 15) return LIFT_NO;
    if (load && !dbl && rd == 15 && sub != 1) return LIFT_NO;
    if ((i->fl & FL_REG) && i->rm == 15) return LIFT_NO;
    const char *base;
    if (rn == 15) { movc(L, "w15", pcv & ~3u); base = "w15"; } else base = W[rn];
    /* effective addresses: w16 = base +- offset */
    int need_off = (i->fl & FL_REG) ? 1 : (i->imm != 0);
    const char *off_addr = base;
    if (need_off || wb) {
        if (i->fl & FL_REG) {
            A32Insn t = *i; t.k = K_RSI;
            Op2 o;
            if (!build_op2(L, &t, pcv, 0, 0, &o)) return LIFT_NO;
            out(L, "\t%s w16, %s, %s", up ? "add" : "sub", base, o.text);
        } else if (i->imm <= 0xfff) out(L, "\t%s w16, %s, #%u", up ? "add" : "sub", base, i->imm);
        else { movc(L, "w17", i->imm); out(L, "\t%s w16, %s, w17", up ? "add" : "sub", base); }
        off_addr = "w16";
    }
    const char *addr = pre ? off_addr : base;
    char amode[48];
    if (load && (wb || dbl)) { out(L, "\tadd x17, x20, %s, uxtw", addr); strcpy(amode, "[x17]"); }
    else snprintf(amode, sizeof amode, "[x20, %s, uxtw]", addr);
    if (dbl) {
        if (load) { if (wb) out(L, "\tmov %s, w16", W[rn]); out(L, "\tldp %s, %s, [x17]", W[rd], W[i->ra]); }
        else { out(L, "\tadd x17, x20, %s, uxtw", addr); out(L, "\tstp %s, %s, [x17]", W[rd], W[i->ra]); if (wb) out(L, "\tmov %s, w16", W[rn]); }
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
    if (rd == 15) { out(L, "\tldr w24, %s", amode); jump_ind(L, "w24", 0); return LIFT_TERM; }
    out(L, "\t%s %s, %s", ld[sub], W[rd], amode);
    return LIFT_OK;
}

static int lift_ldm(Lift *L, const A32Insn *i) {
    unsigned list = i->imm & 0xffff, rn = i->rn, n = (unsigned)__builtin_popcount(list);
    int load = i->sub != 0, wb = (i->fl & FL_W) != 0, pre = (i->fl & FL_P) != 0, up = (i->fl & FL_U) != 0;
    if (rn == 15 || n == 0) return LIFT_NO;
    if (!load && (list >> 15 & 1)) return LIFT_NO;
    int off;   /* byte offset of the first transferred word from the base */
    if (up) off = pre ? 4 : 0; else off = pre ? -4 * (int)n : -4 * (int)n + 4;
    const char *a = W[rn];
    if (off > 0) { out(L, "\tadd w16, %s, #%d", a, off); a = "w16"; }
    else if (off < 0) { out(L, "\tsub w16, %s, #%d", a, -off); a = "w16"; }
    out(L, "\tadd x17, x20, %s, uxtw", a);
    unsigned pos = 0;
    for (unsigned r = 0; r < 16; r++) {
        if (!(list >> r & 1)) continue;
        if (r == 15) { out(L, "\tldr w24, [x17, #%u]", pos * 4); pos++; continue; }
        if (r < 14 && (list >> (r + 1) & 1)) {
            out(L, "\t%s %s, %s, [x17, #%u]", load ? "ldp" : "stp", W[r], W[r + 1], pos * 4); pos += 2; r++;
        } else { out(L, "\t%s %s, [x17, #%u]", load ? "ldr" : "str", W[r], pos * 4); pos++; }
    }
    if (wb && !(load && (list >> rn & 1))) out(L, "\t%s %s, %s, #%u", up ? "add" : "sub", W[rn], W[rn], 4 * n);
    if (load && (list >> 15 & 1)) { jump_ind(L, "w24", 0); return LIFT_TERM; }
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
    if (i->imm) { out(L, "\tadd w16, %s, #%u", base, i->imm); base = "w16"; }
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
    case OP_B: jump_to(L, pcv + i->imm, L->thumb, a64c); *handled_cond = 1; return cond == 15 ? LIFT_TERM : LIFT_OK;
    case OP_CBZ: {
        uint32_t t = pcv + i->imm; uint8_t *f = flagref(L, t, 1);
        if (f && (*f & F_ENT) && chunk_of(t) == L->cur_chunk) { char nm[32]; lname(nm, sizeof nm, t, 1, 1); out(L, "\t%s %s, %s", i->sub ? "cbnz" : "cbz", W[i->rn], nm); }
        else { unsigned lab = L->label++; out(L, "\t%s %s, L%u", i->sub ? "cbz" : "cbnz", W[i->rn], lab); jump_to(L, t, 1, 14); out(L, "L%u:", lab); }
        *handled_cond = 1; return LIFT_OK;
    }
    default: break;
    }
    (void)ret;
    return LIFT_NO;
}

/* Emits the instruction. Returns LIFT_NO when the interpreter has to run it. */
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
    case OP_MOVW: if (i->rd == 15) return LIFT_NO; out(L, "\tmovz %s, #0x%x", W[i->rd], i->imm & 0xffff); return LIFT_OK;
    case OP_MOVT: if (i->rd == 15) return LIFT_NO; out(L, "\tmovk %s, #0x%x, lsl #16", W[i->rd], i->imm & 0xffff); return LIFT_OK;
    case OP_MUL: case OP_MULL: return lift_mul(L, i, setf);
    case OP_DIV: case OP_CLZ: case OP_RBIT: case OP_REV: case OP_REV16: case OP_REVSH: case OP_EXT: case OP_BFX: case OP_BFI: return lift_misc(L, i);
    case OP_SMULH: case OP_SMMUL: return lift_smul(L, i);
    case OP_LDST: return lift_ldst(L, i);
    case OP_LDM: return lift_ldm(L, i);
    case OP_LDREX: case OP_STREX: return lift_excl(L, i);
    case OP_CLREX: out(L, "\tclrex"); return LIFT_OK;
    case OP_B: case OP_CBZ: return lift_branch(L, i, cond, len, handled_cond);
    case OP_BL:
        movc(L, "w14", ret); jump_to(L, pcv + i->imm, L->thumb, 14); return LIFT_TERM;
    case OP_BLX_IMM: {
        uint32_t base = L->thumb ? (pcv & ~3u) : pcv;
        movc(L, "w14", ret); jump_to(L, base + i->imm, !L->thumb, 14); return LIFT_TERM;
    }
    case OP_BX: {
        if (i->rm == 15) { movc(L, "w24", pcv); jump_ind(L, "w24", 0); return LIFT_TERM; }
        jump_ind(L, W[i->rm], 0); return LIFT_TERM;
    }
    case OP_BLX_REG: {
        if (i->rm == 15) return LIFT_NO;
        out(L, "\tmov w24, %s", W[i->rm]); movc(L, "w14", ret); jump_ind(L, "w24", 0); return LIFT_TERM;
    }
    case OP_TB: {
        const char *b = "w15";
        if (i->rn == 15) movc(L, "w15", pcv); else b = W[i->rn];
        if (i->rm == 15) return LIFT_NO;
        if (i->sub == 0) { out(L, "\tadd w16, %s, %s", b, W[i->rm]); out(L, "\tldrb w16, [x20, w16, uxtw]"); }
        else { out(L, "\tadd w16, %s, %s, lsl #1", b, W[i->rm]); out(L, "\tldrh w16, [x20, w16, uxtw]"); }
        movc(L, "w15", pcv);
        out(L, "\tadd w24, w15, w16, lsl #1");
        jump_ind(L, "w24", 1); return LIFT_TERM;
    }
    case OP_MRC:
        if ((i->imm & 15) == 15 && ((i->imm >> 16) & 7) == 0 && ((i->imm >> 12) & 15) == 13 && ((i->imm >> 8) & 15) == 0 && ((i->imm >> 4) & 7) == 3 && i->rd != 15) {
            out(L, "\tldr %s, [x21, #%zu]", W[i->rd], offsetof(A32Cpu, tpidruro)); return LIFT_OK;
        }
        return LIFT_NO;
    default: return LIFT_NO;
    }
}

static void emit_fallback(Lift *L, const A32Insn *i, unsigned len, unsigned it) {
    movc(L, "w24", L->pc | (L->thumb ? 1u : 0u));
    movc(L, "w25", it | len << 8);
    out(L, "\tbl _a32jit_fb");
    L->st->fallback++;
    if (L->in->seed_fb) L->in->seed_fb[(L->pc - L->in->fb_base) / L->in->fb_stride] = 1;
    if (i->op < 64) L->st->fb_by_op[i->op]++;
}

/* ---- regions -------------------------------------------------------------------------------------------------------- */
static void switch_chunk(Lift *L, uint32_t chunk) {
    if (L->o && chunk == L->cur_chunk) return;
    if (L->o) { fclose(L->o); L->o = NULL; }
    char path[1024]; snprintf(path, sizeof path, "%s.%03x.S", L->prefix, chunk);
    L->o = fopen(path, "w");
    if (!L->o) { perror(path); exit(1); }
    if (L->nfiles == L->capfiles) { L->capfiles = L->capfiles ? L->capfiles * 2 : 64; L->files = realloc(L->files, L->capfiles * sizeof *L->files); }
    L->files[L->nfiles++] = strdup(path);
    L->cur_chunk = chunk;
    fputs("\t.text\n\t.p2align 2\n", L->o);
}

static unsigned region_len(Lift *L, uint32_t start, int thumb) {
    unsigned n = 0, it = 0; uint32_t addr = start;
    for (;;) {
        A32Insn i; unsigned len = decode_at(L, addr, thumb, &i);
        if (!len) break;
        n++;
        if (L->in->each_entry) break;
        unsigned cond = insn_cond(&i, thumb, it);
        int term = 0;
        switch (i.op) {
        case OP_B: term = cond == 15; break;
        case OP_BL: case OP_BLX_IMM: case OP_BLX_REG: case OP_BX: case OP_TB: term = cond == 15; break;
        case OP_DP: term = cond == 15 && i.rd == 15 && !(i.sub >= 8 && i.sub <= 11); break;
        case OP_LDST: term = cond == 15 && i.rd == 15 && i.sub == 1; break;
        case OP_LDM: term = cond == 15 && i.sub && (i.imm >> 15 & 1); break;
        case OP_UNDEF: case OP_UNIMPL: term = 1; break;
        default: break;
        }
        if (i.op == OP_IT) it = i.imm; else if (thumb && it) it = it_advance(it);
        if (term) break;
        addr += len;
        uint8_t *f = flagref(L, addr, thumb);
        if (!f || (*f & F_ENT)) break;
    }
    return n ? n : 1;
}

static void emit_region(Lift *L, uint32_t start, int thumb, uint32_t nextkey) {
    char nm[32];
    L->pc = start; L->thumb = thumb;
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
        movc(L, "w24", start | (thumb ? 1u : 0u)); out(L, "\tmovz w25, #0"); out(L, "\tb _a32jit_leave");
        out(L, "L%u:", lab);
    }
    unsigned it = 0; uint32_t addr = start;
    for (;;) {
        A32Insn i; unsigned len = decode_at(L, addr, thumb, &i);
        L->pc = addr;
        if (!len) { movc(L, "w24", addr | (thumb ? 1u : 0u)); out(L, "\tb _a32jit_noblock"); return; }
        unsigned cond = insn_cond(&i, thumb, it);
        int in_it = thumb && it != 0;
        L->st->lifted++;
        if (i.op == OP_IT) { it = i.imm; goto next; }
        {
            /* conditions: lift under a skip branch (except branches, which fold the condition into the jump) */
            unsigned lab = 0; int skipped = 0;
            int is_branch = i.op == OP_B || i.op == OP_CBZ;
            if (cond < 14 && !is_branch) { lab = L->label++; out(L, "\tb.%s L%u", CC[cond ^ 1], lab); skipped = 1; }
            int handled = 0;
            /* a probe: lift into a scratch buffer so a refusal leaves no partial output */
            FILE *real = L->o; char *buf = NULL; size_t bsz = 0;
            L->o = open_memstream(&buf, &bsz);
            int r = lift_insn(L, &i, cond, len, in_it, &handled);
            fclose(L->o); L->o = real;
            if (r != LIFT_NO) { fwrite(buf, 1, bsz, L->o); free(buf); }
            else { free(buf); L->st->lifted--; emit_fallback(L, &i, len, it); r = LIFT_OK; }
            if (skipped) out(L, "L%u:", lab);
            if (r == LIFT_TERM && cond == 15) return;
        }
    next:
        if (L->in->each_entry) { addr += len; movc(L, "w24", addr | (thumb ? 1u : 0u)); out(L, "\tb _a32jit_noblock"); return; }
        if (i.op != OP_IT && thumb && it) it = it_advance(it);
        addr += len;
        uint8_t *f = flagref(L, addr, thumb);
        if (!f) { movc(L, "w24", addr | (thumb ? 1u : 0u)); out(L, "\tb _a32jit_noblock"); return; }
        if (*f & F_ENT) {
            L->pc = addr;
            if ((addr | (thumb ? 1u : 0u)) != nextkey) jump_to(L, addr, thumb, 14);
            return;
        }
    }
}

/* ---- hash table and stubs ------------------------------------------------------------------------------------------ */
typedef struct { uint32_t key; } HEnt;

static const char *const STUBS =
"\t.macro A32_SPILL\n"
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
    if (!o) { perror(path); exit(1); }
    L->o = o;
    unsigned bits = 4;
    while ((1u << bits) < nkeys * 2) bits++;
    unsigned size = 1u << bits;
    uint32_t *slot = calloc(size, sizeof *slot);
    uint8_t *used = calloc(size, 1);
    for (unsigned k = 0; k < nkeys; k++) {
        unsigned h = (uint32_t)(keys[k] * 0x9E3779B1u) >> (32 - bits);
        while (used[h]) h = (h + 1) & (size - 1);
        used[h] = 1; slot[h] = keys[k];
    }
    size_t n_off = offsetof(A32Cpu, n), c_off = offsetof(A32Cpu, c);
    fprintf(o, "\t.text\n\t.p2align 2\n");
    fprintf(o, STUBS, n_off, c_off, n_off, c_off);
    fprintf(o, "\t.globl _a32jit_hash_bits\n\t.globl _a32jit_hash\n\t.globl _a32jit_hooks\n\t.globl _a32jit_enter\n\t.globl _a32jit_ind\n");
    fprintf(o, "\t.p2align 2\n"
        /* int a32jit_enter(A32Cpu *c, int64_t budget): runs lifted code from c->r[15] until it leaves; returns the A32Exit or EXIT_NOBLOCK */
        "_a32jit_enter:\n\tstp x29, x30, [sp, #-96]!\n\tstp x19, x20, [sp, #16]\n\tstp x21, x22, [sp, #32]\n\tstp x23, x24, [sp, #48]\n\tstp x25, x26, [sp, #64]\n\tstp x27, x28, [sp, #80]\n"
        "\tmov x21, x0\n\tmov x19, x1\n\tldr x20, [x21, #%zu]\n\tadrp x22, _a32jit_hash@PAGE\n\tadd x22, x22, _a32jit_hash@PAGEOFF\n"
        "\tA32_RELOAD\n\tldr w24, [x21, #%zu]\n\tldr w16, [x21, #%zu]\n\torr w24, w24, w16\n\tb _a32jit_ind\n",
        offsetof(A32Cpu, mem), offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, thumb));
    fprintf(o,
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
        /* fallback: run the one instruction at w24 (bit 0 = thumb) in the interpreter. w25 = itstate | length << 8. */
        "\t.globl _a32jit_fb\n_a32jit_fb:\n\tmov x27, x30\n\tA32_SPILL\n"
        "\tand w15, w24, #1\n\tstr w15, [x21, #%zu]\n\tand w16, w24, #0xfffffffe\n\tstr w16, [x21, #%zu]\n\tand w17, w25, #0xff\n\tstr w17, [x21, #%zu]\n"
        "\tadd w26, w16, w25, lsr #8\n\torr w26, w26, w15\n"
        "\tadrp x16, _a32jit_hooks@PAGE\n\tldr x16, [x16, _a32jit_hooks@PAGEOFF]\n\tmov x0, x21\n\tblr x16\n"
        "\tcbnz w0, 4f\n\tA32_RELOAD\n"
        "\tldr w16, [x21, #%zu]\n\tldr w17, [x21, #%zu]\n\torr w24, w16, w17\n\teor w16, w24, w26\n\tcbnz w16, _a32jit_ind\n\tret x27\n"
        "4:\tmov w25, w0\n\tb _a32jit_ret\n",
        offsetof(A32Cpu, thumb), offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, itstate),
        offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, thumb));
    fprintf(o,
        /* a branch into the host-implemented import stubs (guest address 0xF00xxxxx): call the host function right here and continue at the guest's lr,
         * without leaving the lifted code */
        "_a32jit_hle:\n\tA32_SPILL\n\tmov x0, x21\n\tmov w1, w24\n\tadrp x16, _a32jit_hooks@PAGE\n\tldr x16, [x16, _a32jit_hooks@PAGEOFF + 8]\n\tblr x16\n"
        "\tcbnz w0, 6f\n\tA32_RELOAD\n\tldr w24, [x21, #%zu]\n\tldr w16, [x21, #%zu]\n\torr w24, w24, w16\n\tb _a32jit_ind\n"
        "6:\tmovz w25, #1\n\tb _a32jit_ret\n", offsetof(A32Cpu, r) + 60, offsetof(A32Cpu, thumb));
    fprintf(o, "\t.data\n\t.p2align 4\n_a32jit_hooks:\n\t.quad 0, 0\n\t.p2align 4\n_a32jit_hash_bits:\n\t.quad %u\n", bits);
    fprintf(o, "\t.section __DATA,__const\n\t.p2align 4\n_a32jit_hash:\n");
    for (unsigned h = 0; h < size; h++) {
        if (!used[h]) { fprintf(o, "\t.quad 0, 0\n"); continue; }
        char nm[32]; lname(nm, sizeof nm, slot[h] & ~1u, slot[h] & 1, 0);
        fprintf(o, "\t.quad 0x%x, %s\n", slot[h], nm);
    }
    fclose(o);
    free(slot); free(used);
}

int a32lift_run(const LiftInput *in, const char *prefix, char ***out_files, LiftStats *st) {
    Lift L; memset(&L, 0, sizeof L);
    L.in = in; L.prefix = prefix; L.st = st;
    memset(st, 0, sizeof *st);
    L.mf = calloc(in->nmods, sizeof *L.mf);
    for (unsigned m = 0; m < in->nmods; m++) {
        L.mf[m].base = in->mods[m].base; L.mf[m].text_end = in->mods[m].text_end;
        L.mf[m].fa = calloc(in->mods[m].text_end / 4 + 2, 1); L.mf[m].ft = calloc(in->mods[m].text_end / 2 + 2, 1);
    }
    if (in->each_entry) { for (unsigned s = 0; s < in->nseeds; s++) { uint8_t *f = flagref(&L, in->seeds[s] & ~1u, in->seeds[s] & 1); if (f) *f |= F_ENT | F_CHK | F_VIS; } }
    else discover(&L);
    /* collect entries in address order */
    uint32_t *keys = NULL; unsigned nkeys = 0, cap = 0;
    for (unsigned m = 0; m < in->nmods; m++) {
        for (uint32_t off = 0; off < in->mods[m].text_end; off += 2) {
            for (int th = 0; th < 2; th++) {
                if (!th && (off & 2)) continue;
                uint8_t f = th ? L.mf[m].ft[off >> 1] : L.mf[m].fa[off >> 2];
                if (!(f & F_ENT)) continue;
                if (nkeys == cap) { cap = cap ? cap * 2 : 1024; keys = realloc(keys, cap * sizeof *keys); }
                keys[nkeys++] = (in->mods[m].base + off) | (uint32_t)th;
            }
        }
    }
    for (unsigned k = 0; k < nkeys; k++) {
        uint32_t a = keys[k] & ~1u; int th = keys[k] & 1;
        switch_chunk(&L, chunk_of(a));
        /* the next key is the physical successor in this file: a region that is cut there falls through into it */
        uint32_t nk = k + 1 < nkeys && chunk_of(keys[k + 1] & ~1u) == chunk_of(a) ? keys[k + 1] : 0xffffffffu;
        L.pc = a; L.thumb = th;
        emit_region(&L, a, th, nk);
    }
    if (L.o) fclose(L.o);
    char tab[1024]; snprintf(tab, sizeof tab, "%s.tab.S", prefix);
    emit_tab(&L, tab, keys, nkeys);
    if (L.nfiles == L.capfiles) { L.capfiles++; L.files = realloc(L.files, L.capfiles * sizeof *L.files); }
    L.files[L.nfiles++] = strdup(tab);
    *out_files = L.files;
    for (unsigned m = 0; m < in->nmods; m++) { free(L.mf[m].fa); free(L.mf[m].ft); }
    free(L.mf); free(L.stack); free(keys);
    return (int)L.nfiles;
}
