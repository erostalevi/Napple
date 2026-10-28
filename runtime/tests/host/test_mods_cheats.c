/* Cheat engine (mods_cheats.c): the same vectors as crates/napple-mods/src/cheats.rs tests, so the two interpreters stay in step. Portable. */
#include "../check.h"
#include "mods_cheats.h"
#include <stdint.h>

typedef struct { int r; uint64_t a; uint8_t v; } B;
typedef struct { B b[512]; int n; } Mem;
static B *find(Mem *m, int r, uint64_t a) {
    for (int i = 0; i < m->n; i++) if (m->b[i].r == r && m->b[i].a == a) return &m->b[i];
    return NULL;
}
static int mrd(void *c, int r, uint64_t a, int w, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < w; i++) {
        B *b = find(c, r, a + (uint64_t)i);
        if (!b) return 0;
        v |= (uint64_t)b->v << (8 * i);
    }
    *out = v;
    return 1;
}
static int mwr(void *c, int r, uint64_t a, int w, uint64_t v) {
    Mem *m = c;
    for (int i = 0; i < w; i++) {
        B *b = find(m, r, a + (uint64_t)i);
        if (!b) {
            if (m->n >= 512) return 0;
            b = &m->b[m->n++];
            b->r = r;
            b->a = a + (uint64_t)i;
        }
        b->v = (uint8_t)(v >> (8 * i));
    }
    return 1;
}
static const McMem OPS = {mrd, mwr};
static uint64_t get(Mem *m, int r, uint64_t a, int w) {
    uint64_t v = ~0ull;
    mrd(m, r, a, w, &v);
    return v;
}
static McStats run_words(const uint32_t *w, size_t n, Mem *m, int *ok) {
    McInsn *ops;
    size_t no;
    char why[160] = "";
    *ok = mc_decode(w, n, &ops, &no, why, sizeof why);
    if (!*ok && getenv("MC_VERBOSE")) printf("  decode: %s\n", why);
    McStats s = mc_run(ops, no, &OPS, m);
    free(ops);
    return s;
}
#define RUN(arr, mem, okv) run_words(arr, sizeof(arr) / sizeof(arr[0]), mem, okv)
static int decodes(const uint32_t *w, size_t n) {
    McInsn *ops;
    size_t no;
    char why[160];
    int ok = mc_decode(w, n, &ops, &no, why, sizeof why);
    free(ops);
    return ok;
}

int main(void) {
    int ok;
    Mem m;
    /* the original subset: store + conditional/else */
    const char *file = "[Infinite coins]\n04000000 00123456 0000270F\n\n{Master}\n04000000 00000010 00000001\n"
                       "[Cond]\n14050000 00001000 00000001\n04000000 00002000 000000AA\n21000000\n04000000 00002000 000000BB\n20000000\n"
                       "[Wide]\n08000000 00003000 11223344 55667788\n[Bad]\n80000000\n";
    char err[128] = "";
    ModsCheats *cf = mods_cheats_parse(file, strlen(file), err, sizeof err);
    CHECK(cf && cf->n == 5, "parses 5 cheats %s", err);
    if (!cf) return 1;
    CHECK(cf->c[1].master && !cf->c[0].master, "master flag");
    CHECK(!cf->c[0].unsupported && !cf->c[2].unsupported && cf->c[4].unsupported, "only [Bad] is unsupported");
    memset(&m, 0, sizeof m);
    mwr(&m, MC_MAIN, 0x1000, 4, 1);
    McStats st = {0};
    mods_cheats_run_all(cf, &OPS, &m, &st);
    CHECK(get(&m, MC_MAIN, 0x2000, 1) == 0xAA, "condition true takes the then block");
    CHECK(get(&m, MC_MAIN, 0x123456, 2) == 0x270F && get(&m, MC_MAIN, 0x10, 4) == 1 && get(&m, MC_MAIN, 0x3000, 8) == 0x1122334455667788ull, "stores incl. 8 byte");
    CHECK(st.stores == 4, "4 stores, got %u", st.stores);
    mwr(&m, MC_MAIN, 0x1000, 4, 2);
    mods_cheats_run_all(cf, &OPS, &m, &st);
    CHECK(get(&m, MC_MAIN, 0x2000, 1) == 0xBB, "condition false takes the else block");
    mods_cheats_free(cf);

    /* offset register */
    memset(&m, 0, sizeof m);
    const uint32_t a1[] = {0x40010000, 0, 0x100, 0x01010000, 0x10, 0xAB};
    RUN(a1, &m, &ok);
    CHECK(ok && get(&m, MC_MAIN, 0x110, 1) == 0xAB, "offset register store");
    /* loop on r2 = 3 runs the body 4 times */
    memset(&m, 0, sizeof m);
    const uint32_t a2[] = {0x30020000, 3, 0x04010000, 0x10, 0x77, 0x31020000};
    st = RUN(a2, &m, &ok);
    CHECK(ok && st.stores == 4, "loop runs count+1 times, got %u", st.stores);
    /* store with increment */
    memset(&m, 0, sizeof m);
    const uint32_t a3[] = {0x40060000, 0, 0x30, 0x68061000, 0, 0x1234, 0x61061000, 0, 0x99};
    st = RUN(a3, &m, &ok);
    CHECK(ok && st.stores == 2 && get(&m, MC_ABS, 0x30, 8) == 0x1234 && get(&m, MC_ABS, 0x38, 1) == 0x99, "increment store");
    /* store with offset register */
    memset(&m, 0, sizeof m);
    const uint32_t a4[] = {0x40070000, 0, 0x100, 0x40080000, 0, 0x20, 0x61070180, 0, 0x5A};
    RUN(a4, &m, &ok);
    CHECK(ok && get(&m, MC_ABS, 0x120, 1) == 0x5A, "offset register on a register store");
    /* arithmetic wraps at the width */
    const uint32_t a5[] = {0x40050000, 0, 0xFF, 0x71050000, 2, 0x40060000, 0, 0x400, 0x68060000, 0, 0};
    CHECK(decodes(a5, 11), "arithmetic decodes");
    /* pointer chain */
    const uint32_t a6[] = {0x58110000, 0x8, 0x54011000, 0x4};
    memset(&m, 0, sizeof m);
    mwr(&m, MC_HEAP, 0x8, 8, 0x5000);
    mwr(&m, MC_ABS, 0x5004, 4, 0xDEAD);
    st = RUN(a6, &m, &ok);
    CHECK(ok && st.failed == 0, "pointer chain loads");
    memset(&m, 0, sizeof m);
    st = RUN(a6, &m, &ok);
    CHECK(ok && st.failed == 2, "unmapped loads are counted, got %u", st.failed);
    /* a false conditional skips a loop (and its end) */
    const uint32_t a7[] = {0x14150000, 0x1000, 1, 0x30020000, 1000, 0x31020000, 0x20000000};
    memset(&m, 0, sizeof m);
    mwr(&m, MC_HEAP, 0x1000, 4, 0);
    st = RUN(a7, &m, &ok);
    CHECK(ok && !st.aborted, "skipped loop");
    /* runaway loop is bounded */
    const uint32_t a8[] = {0x30000000, 0xFFFFFFFFu, 0x74010000, 1, 0x31000000};
    memset(&m, 0, sizeof m);
    st = RUN(a8, &m, &ok);
    CHECK(ok && st.aborted, "runaway loop aborts");
    /* structure errors */
    const uint32_t e1[] = {0x30000000, 2};
    const uint32_t e2[] = {0x31000000};
    const uint32_t e3[] = {0x30000000, 2, 0x31010000};
    const uint32_t e4[] = {0x30000000, 2, 0x20000000, 0x31000000};
    const uint32_t e5[] = {0x79000000, 1};
    const uint32_t e6[] = {0xA0000000};
    const uint32_t e7[] = {0xFF000000, 0xFF100000};
    const uint32_t e8[] = {0x04000000};
    CHECK(!decodes(e1, 2) && !decodes(e2, 1) && !decodes(e3, 3) && !decodes(e4, 4) && !decodes(e5, 2) && !decodes(e6, 1) && !decodes(e8, 1), "structure errors are refused");
    CHECK(decodes(e7, 2), "pause / resume are no-ops");
    /* parse errors */
    CHECK(!mods_cheats_parse("04000000", 8, err, sizeof err), "opcode before name");
    CHECK(!mods_cheats_parse("[x\n", 3, err, sizeof err), "unterminated name");
    CHECK(!mods_cheats_parse("[x]\nZZ000000\n", 13, err, sizeof err), "bad hex");
    CHECK(!mods_cheats_parse("[x]\n0400000\n", 12, err, sizeof err), "short word");
    /* shared fixture with crates/napple-mods (cwd = runtime/) */
    FILE *vf = fopen("../docs/mods/examples/cheat-vectors.txt", "rb");
    CHECK(vf != NULL, "shared vector file");
    if (vf) {
        static char vb[8192];
        size_t vn = fread(vb, 1, sizeof vb - 1, vf);
        fclose(vf);
        ModsCheats *v = mods_cheats_parse(vb, vn, err, sizeof err);
        CHECK(v && v->n == 7, "7 shared cheats %s", err);
        for (size_t i = 0; v && i < v->n; i++)
            CHECK((v->c[i].unsupported != NULL) == !strcmp(v->c[i].name, "Unsupported"), "shared vector [%s]: %s", v->c[i].name, v->c[i].unsupported ? v->c[i].unsupported : "supported");
        mods_cheats_free(v);
    }
    /* corpus: random words never crash or hang */
    uint64_t s = 0xdeadbeefcafef00dull;
    for (int i = 0; i < 20000; i++) {
        uint32_t w[12];
        size_t n = 0;
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        n = (size_t)(s % 12);
        for (size_t k = 0; k < n; k++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; w[k] = (uint32_t)s & 0x7FFFFFFF; }
        memset(&m, 0, sizeof m);
        run_words(w, n, &m, &ok);
    }
    printf("test_mods_cheats: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
