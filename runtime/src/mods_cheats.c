#include "mods_cheats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHEATS 4096
#define MAX_WORDS (1u << 16)
#define WHYF(...) do { if (why && cap) snprintf(why, cap, __VA_ARGS__); } while (0)

static int width_ok(unsigned w) { return w == 1 || w == 2 || w == 4 || w == 8; }

int mc_decode(const uint32_t *w, size_t n, McInsn **ops_out, size_t *nops_out, char *why, size_t cap) {
    McInsn *ops = calloc(n ? n : 1, sizeof *ops);
    size_t no = 0, i = 0;
    int blocks[256]; /* 0 = conditional, 1 + reg = loop on reg */
    int nb = 0, ok = 1;
    if (!ops) return 0;
    while (i < n && ok) {
        uint32_t x0 = w[i], code = x0 >> 28, t = (x0 >> 24) & 0xF, m = (x0 >> 20) & 0xF, x = (x0 >> 16) & 0xF;
        McInsn *o = &ops[no];
        memset(o, 0, sizeof *o);
        if (code == 0 || code == 1) {
            size_t k = t == 8 ? 4 : 3;
            if (!width_ok(t)) { WHYF("word %zu: bad width %u", i, t); ok = 0; break; }
            if (m > 3) { WHYF("word %zu: bad memory type %u", i, m); ok = 0; break; }
            if (i + k > n) { WHYF("word %zu: truncated opcode", i); ok = 0; break; }
            if (x0 & 0xFF00) { WHYF("word %zu: reserved bits set", i); ok = 0; break; }
            o->width = (uint8_t)t;
            o->region = (uint8_t)m;
            o->addr = ((uint64_t)(x0 & 0xFF) << 32) | w[i + 1];
            o->value = w[i + 2];
            if (t == 8) o->value = (o->value << 32) | w[i + 3];
            if (code == 0) {
                o->type = MC_STORE;
                o->reg = (uint8_t)x;
            } else {
                if (x < 1 || x > 6) { WHYF("word %zu: bad condition %u", i, x); ok = 0; break; }
                if (nb >= 256) { WHYF("word %zu: blocks nested too deep", i); ok = 0; break; }
                o->type = MC_IF;
                o->cond = (uint8_t)x;
                blocks[nb++] = 0;
            }
            i += k;
        } else if (code == 2 && (x0 == 0x20000000u || x0 == 0x21000000u)) {
            if (!nb || blocks[nb - 1] != 0) { WHYF("word %zu: end/else without a conditional", i); ok = 0; break; }
            if (x0 == 0x20000000u) { nb--; o->type = MC_END; }
            else o->type = MC_ELSE;
            i += 1;
        } else if (code == 3 && (x0 & 0xFF00FFFFu) == 0x30000000u && i + 2 <= n) {
            if (nb >= 256) { WHYF("word %zu: blocks nested too deep", i); ok = 0; break; }
            o->type = MC_LOOP;
            o->reg = (uint8_t)x;
            o->count = w[i + 1];
            blocks[nb++] = 1 + (int)x;
            i += 2;
        } else if (code == 3 && (x0 & 0xFF00FFFFu) == 0x31000000u) {
            if (!nb || blocks[nb - 1] != 1 + (int)x) { WHYF("word %zu: loop end without its loop start", i); ok = 0; break; }
            nb--;
            o->type = MC_ENDLOOP;
            o->reg = (uint8_t)x;
            i += 1;
        } else if (code == 4 && (x0 & 0xFF00FFFFu) == 0x40000000u && i + 3 <= n) {
            o->type = MC_LOADIMM;
            o->reg = (uint8_t)x;
            o->value = ((uint64_t)w[i + 1] << 32) | w[i + 2];
            i += 3;
        } else if (code == 5 && ((x0 >> 8) & 0xF) == 0 && i + 2 <= n) {
            unsigned rel = (x0 >> 12) & 0xF;
            if (!width_ok(t) || rel > 1) { WHYF("word %zu: bad load width or source", i); ok = 0; break; }
            if (m > 3) { WHYF("word %zu: bad memory type %u", i, m); ok = 0; break; }
            if (rel == 1 && m != 0) { WHYF("word %zu: register-relative load takes no memory type", i); ok = 0; break; }
            o->type = MC_LOADMEM;
            o->width = (uint8_t)t;
            o->region = rel ? MC_ABS : (uint8_t)m;
            o->reg = (uint8_t)x;
            o->rel = (uint8_t)rel;
            o->addr = ((uint64_t)(x0 & 0xFF) << 32) | w[i + 1];
            i += 2;
        } else if (code == 6 && m == 0 && (x0 & 0xF) == 0 && ((x0 >> 12) & 0xF) <= 1 && ((x0 >> 8) & 0xF) <= 1 && i + 3 <= n) {
            unsigned o_t = (x0 >> 8) & 0xF;
            if (!width_ok(t)) { WHYF("word %zu: bad width %u", i, t); ok = 0; break; }
            if (o_t == 0 && ((x0 >> 4) & 0xF) != 0) { WHYF("word %zu: reserved bits set", i); ok = 0; break; }
            o->type = MC_STOREREG;
            o->width = (uint8_t)t;
            o->reg = (uint8_t)x;
            o->inc = (uint8_t)((x0 >> 12) & 0xF);
            o->has_off = (uint8_t)o_t;
            o->off = (uint8_t)((x0 >> 4) & 0xF);
            o->value = ((uint64_t)w[i + 1] << 32) | w[i + 2];
            i += 3;
        } else if (code == 7 && m == 0 && (x0 & 0xFFF) == 0) {
            unsigned c = (x0 >> 12) & 0xF;
            size_t k = t == 8 ? 3 : 2;
            if (c > 4) { WHYF("word %zu: bad arithmetic op %u", i, c); ok = 0; break; }
            if (!width_ok(t)) { WHYF("word %zu: bad width %u", i, t); ok = 0; break; }
            if (i + k > n) { WHYF("word %zu: truncated opcode", i); ok = 0; break; }
            o->type = MC_ARITH;
            o->width = (uint8_t)t;
            o->reg = (uint8_t)x;
            o->aop = (uint8_t)c;
            o->value = w[i + 1];
            if (t == 8) o->value = (o->value << 32) | w[i + 2];
            i += k;
        } else if (x0 == 0xFF000000u || x0 == 0xFF100000u) { /* pause / resume: nothing to do for a frame-synchronous runner */
            i += 1;
            continue;
        } else {
            WHYF("word %zu: code type %X (%08X) is not in the supported subset", i, code, x0);
            ok = 0;
            break;
        }
        no++;
    }
    if (ok && nb) { WHYF("unterminated conditional or loop block"); ok = 0; }
    *ops_out = ops;
    *nops_out = no;
    return ok;
}

void mods_cheats_free(ModsCheats *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n; i++) {
        free(c->c[i].ops);
        free(c->c[i].unsupported);
    }
    free(c->c);
    free(c);
}

ModsCheats *mods_cheats_parse(const char *text, size_t len, char *why, size_t cap) {
    ModsCheats *cf = calloc(1, sizeof *cf);
    uint32_t **words = NULL;
    size_t *nw = NULL, *ww = NULL;
    size_t pos = 0, ln = 0, ncap = 0;
    int bad = 0;
    if (!cf) return NULL;
    while (pos < len && !bad) {
        size_t e = pos;
        while (e < len && text[e] != '\n') e++;
        ln++;
        const char *l = text + pos, *end = text + e;
        pos = e < len ? e + 1 : e;
        while (l < end && (*l == ' ' || *l == '\t' || *l == '\r')) l++;
        while (end > l && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
        if (l == end || (end - l >= 2 && l[0] == '/' && l[1] == '/')) continue;
        if (*l == '[' || *l == '{') {
            char close = *l == '[' ? ']' : '}';
            if (end[-1] != close || end - l < 2) { WHYF("line %zu: unterminated name", ln); bad = 1; break; }
            const char *a = l + 1, *b = end - 1;
            while (a < b && (*a == ' ' || *a == '\t')) a++;
            while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
            if (b == a || b - a > 64) { WHYF("line %zu: cheat name must be 1..=64 bytes", ln); bad = 1; break; }
            if (cf->n >= MAX_CHEATS) { WHYF("too many cheats"); bad = 1; break; }
            if (cf->n == ncap) {
                ncap = ncap ? ncap * 2 : 16;
                cf->c = realloc(cf->c, ncap * sizeof *cf->c);
                words = realloc(words, ncap * sizeof *words);
                nw = realloc(nw, ncap * sizeof *nw);
                ww = realloc(ww, ncap * sizeof *ww);
                if (!cf->c || !words || !nw || !ww) { bad = 1; break; }
            }
            McCheat *c = &cf->c[cf->n];
            memset(c, 0, sizeof *c);
            memcpy(c->name, a, (size_t)(b - a));
            c->master = *l == '{';
            words[cf->n] = NULL;
            nw[cf->n] = ww[cf->n] = 0;
            cf->n++;
            continue;
        }
        if (!cf->n) { WHYF("line %zu: opcode line before any [name]", ln); bad = 1; break; }
        size_t k = cf->n - 1;
        while (l < end) {
            while (l < end && (*l == ' ' || *l == '\t')) l++;
            const char *s = l;
            while (l < end && *l != ' ' && *l != '\t') l++;
            if (s == l) break;
            uint32_t v = 0;
            if (l - s != 8) { WHYF("line %zu: not an 8-digit hex word", ln); bad = 1; break; }
            for (const char *p = s; p < l; p++) {
                int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
                if (d < 0) { WHYF("line %zu: not an 8-digit hex word", ln); bad = 1; break; }
                v = v << 4 | (uint32_t)d;
            }
            if (bad) break;
            if (nw[k] >= MAX_WORDS) { WHYF("line %zu: cheat too long", ln); bad = 1; break; }
            if (nw[k] == ww[k]) {
                ww[k] = ww[k] ? ww[k] * 2 : 16;
                words[k] = realloc(words[k], ww[k] * sizeof **words);
                if (!words[k]) { bad = 1; break; }
            }
            words[k][nw[k]++] = v;
        }
    }
    if (!bad)
        for (size_t k = 0; k < cf->n; k++) {
            char r[160] = "";
            if (!mc_decode(words[k], nw[k], &cf->c[k].ops, &cf->c[k].nops, r, sizeof r)) cf->c[k].unsupported = strdup(r[0] ? r : "unsupported");
        }
    for (size_t k = 0; k < cf->n; k++) free(words[k]);
    free(words);
    free(nw);
    free(ww);
    if (bad) {
        for (size_t k = 0; k < cf->n; k++) { free(cf->c[k].ops); free(cf->c[k].unsupported); }
        free(cf->c);
        free(cf);
        return NULL;
    }
    return cf;
}

static uint64_t trunc_w(uint64_t v, unsigned w) { return w >= 8 ? v : v & ((1ull << (8 * w)) - 1); }

McStats mc_run(const McInsn *ops, size_t n, const McMem *mem, void *ctx) {
    McStats st = {0};
    uint64_t regs[16] = {0};
    size_t tops[16] = {0}, ip = 0;
    struct { uint8_t parent, res; } stack[256];
    int sp = 0;
    unsigned steps = 0;
#define ACTIVE() (sp == 0 || (stack[sp - 1].parent && stack[sp - 1].res))
    while (ip < n) {
        if (++steps > MC_MAX_STEPS) { st.aborted = 1; break; }
        const McInsn *o = &ops[ip++];
        switch (o->type) {
        case MC_STORE:
            if (ACTIVE()) {
                if (mem->write(ctx, o->region, o->addr + regs[o->reg & 15], o->width, trunc_w(o->value, o->width))) st.stores++;
                else st.failed++;
            }
            break;
        case MC_IF: {
            int parent = ACTIVE(), res = 0;
            if (parent) {
                uint64_t v;
                if (mem->read(ctx, o->region, o->addr, o->width, &v)) {
                    uint64_t c = trunc_w(o->value, o->width);
                    res = o->cond == MC_GT ? v > c : o->cond == MC_GE ? v >= c : o->cond == MC_LT ? v < c : o->cond == MC_LE ? v <= c : o->cond == MC_EQ ? v == c : v != c;
                } else st.failed++;
            }
            if (sp < 256) { stack[sp].parent = (uint8_t)parent; stack[sp].res = (uint8_t)res; sp++; }
            break;
        }
        case MC_ELSE:
            if (sp) stack[sp - 1].res = !stack[sp - 1].res;
            break;
        case MC_END:
            if (sp) sp--;
            break;
        case MC_LOOP:
            if (ACTIVE()) { regs[o->reg & 15] = o->count; tops[o->reg & 15] = ip; }
            break;
        case MC_ENDLOOP:
            if (ACTIVE() && regs[o->reg & 15]) { regs[o->reg & 15]--; ip = tops[o->reg & 15]; }
            break;
        case MC_LOADIMM:
            if (ACTIVE()) regs[o->reg & 15] = o->value;
            break;
        case MC_LOADMEM:
            if (ACTIVE()) {
                uint64_t v, a = o->rel ? regs[o->reg & 15] + o->addr : o->addr;
                if (mem->read(ctx, o->region, a, o->width, &v)) regs[o->reg & 15] = v;
                else st.failed++;
            }
            break;
        case MC_STOREREG:
            if (ACTIVE()) {
                uint64_t a = regs[o->reg & 15] + (o->has_off ? regs[o->off & 15] : 0);
                if (mem->write(ctx, MC_ABS, a, o->width, trunc_w(o->value, o->width))) st.stores++;
                else st.failed++;
                if (o->inc) regs[o->reg & 15] += o->width;
            }
            break;
        case MC_ARITH:
            if (ACTIVE()) {
                uint64_t a = trunc_w(regs[o->reg & 15], o->width), v = o->value, r = 0;
                switch (o->aop) {
                case MC_ADD: r = a + v; break;
                case MC_SUB: r = a - v; break;
                case MC_MUL: r = a * v; break;
                case MC_LSH: r = (uint32_t)v >= 64 ? 0 : a << (uint32_t)v; break;
                default: r = (uint32_t)v >= 64 ? 0 : a >> (uint32_t)v; break;
                }
                regs[o->reg & 15] = trunc_w(r, o->width);
            }
            break;
        }
    }
#undef ACTIVE
    return st;
}

void mods_cheats_run_all(const ModsCheats *c, const McMem *mem, void *ctx, McStats *total) {
    if (!c) return;
    for (size_t i = 0; i < c->n; i++) {
        if (c->c[i].unsupported) continue;
        McStats s = mc_run(c->c[i].ops, c->c[i].nops, mem, ctx);
        total->stores += s.stores;
        total->failed += s.failed;
        total->aborted |= s.aborted;
    }
}
