/* Pins the memory ordering the lifter gives the AArch32 exclusives and acquire / release accesses (core-ordering-kernel, 2026-11-02).
 *
 *   cc -std=gnu11 -O2 -DNAPPLE_A32_INTERP -Isrc/a32 -o excltest tests/a32jit/excltest.c src/a32jit/a32lift.c src/a32/a32_*.c(minus module/proc/hle) && ./excltest
 *
 * Every ldrex / strex form (word, byte, half, dword; plain and ldaex / stlex; ARM and Thumb) is lifted as an entry of its own and the emitted assembly
 * must hold the acquire / release host exclusive (ldaxr / stlxr, ldaxp / stlxp) and never a plain one (ldxr / stxr, ldxp / stxp): a lock taken
 * with a plain pair lets an Apple core run the protected loads before the store-exclusive. lda / stl must be the host's RCsc ldar / stlr (a release
 * store followed by an acquire load stays in order; a plain access plus a one-sided barrier does not give that). Each instruction must be lifted
 * natively (no fallback to the out-of-line semantics). The semantics themselves are covered by difftest (family "sync"). */
#include "../../src/a32jit/a32lift.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE_BASE 0x1000000u

typedef struct {
    const char *name;
    int thumb;
    uint32_t w;            /* ARM word, or Thumb hw1 | hw2 << 16 */
    const char *want[3];   /* host instructions that must appear (with the trailing space or tab of the mnemonic) */
} Case;

#define T(h1, h2) ((uint32_t)(h1) | (uint32_t)(h2) << 16)
static const Case CASES[] = {
    /* ARM */
    {"ldrex", 0, 0xe1921f9f, {"\tldaxr w1, [x17]"}},
    {"ldrexb", 0, 0xe1d21f9f, {"\tldaxrb w1, [x17]"}},
    {"ldrexh", 0, 0xe1f21f9f, {"\tldaxrh w1, [x17]"}},
    {"ldrexd", 0, 0xe1b20f9f, {"\tldaxp w0, w1, [x17]"}},
    {"strex", 0, 0xe1823f91, {"\tstlxr w3, w1, [x17]"}},
    {"strexb", 0, 0xe1c23f91, {"\tstlxrb w3, w1, [x17]"}},
    {"strexh", 0, 0xe1e23f91, {"\tstlxrh w3, w1, [x17]"}},
    {"strexd", 0, 0xe1a23f90, {"\tstlxp w3, w0, w1, [x17]"}},
    {"ldaex", 0, 0xe1921e9f, {"\tldaxr w1, [x17]"}},
    {"ldaexb", 0, 0xe1d21e9f, {"\tldaxrb w1, [x17]"}},
    {"ldaexh", 0, 0xe1f21e9f, {"\tldaxrh w1, [x17]"}},
    {"ldaexd", 0, 0xe1b20e9f, {"\tldaxp w0, w1, [x17]"}},
    {"stlex", 0, 0xe1823e91, {"\tstlxr w3, w1, [x17]"}},
    {"stlexb", 0, 0xe1c23e91, {"\tstlxrb w3, w1, [x17]"}},
    {"stlexh", 0, 0xe1e23e91, {"\tstlxrh w3, w1, [x17]"}},
    {"stlexd", 0, 0xe1a23e90, {"\tstlxp w3, w0, w1, [x17]"}},
    {"ldrex cond", 0, 0x01921f9f, {"\tldaxr w1, [x17]"}},
    {"lda", 0, 0xe1921c9f, {"\tldar w1, [x17]"}},
    {"ldab", 0, 0xe1d21c9f, {"\tldarb w1, [x17]"}},
    {"ldah", 0, 0xe1f21c9f, {"\tldarh w1, [x17]"}},
    {"stl", 0, 0xe182fc91, {"\tstlr w1, [x17]"}},
    {"stlb", 0, 0xe1c2fc91, {"\tstlrb w1, [x17]"}},
    {"stlh", 0, 0xe1e2fc91, {"\tstlrh w1, [x17]"}},
    /* Thumb */
    {"t ldrex", 1, T(0xe852, 0x1f00), {"\tldaxr w1, [x17]"}},
    {"t ldrex #8", 1, T(0xe852, 0x1f02), {"\tadd w16, w2, #8", "\tldaxr w1, [x17]"}},
    {"t ldrexb", 1, T(0xe8d2, 0x1f4f), {"\tldaxrb w1, [x17]"}},
    {"t ldrexh", 1, T(0xe8d2, 0x1f5f), {"\tldaxrh w1, [x17]"}},
    {"t ldrexd", 1, T(0xe8d2, 0x017f), {"\tldaxp w0, w1, [x17]"}},
    {"t strex", 1, T(0xe842, 0x1300), {"\tstlxr w3, w1, [x17]"}},
    {"t strex #4", 1, T(0xe842, 0x1301), {"\tadd w16, w2, #4", "\tstlxr w3, w1, [x17]"}},
    {"t strexb", 1, T(0xe8c2, 0x1f43), {"\tstlxrb w3, w1, [x17]"}},
    {"t strexh", 1, T(0xe8c2, 0x1f53), {"\tstlxrh w3, w1, [x17]"}},
    {"t strexd", 1, T(0xe8c2, 0x0173), {"\tstlxp w3, w0, w1, [x17]"}},
    {"t ldaex", 1, T(0xe8d2, 0x1fef), {"\tldaxr w1, [x17]"}},
    {"t ldaexb", 1, T(0xe8d2, 0x1fcf), {"\tldaxrb w1, [x17]"}},
    {"t ldaexh", 1, T(0xe8d2, 0x1fdf), {"\tldaxrh w1, [x17]"}},
    {"t ldaexd", 1, T(0xe8d2, 0x01ff), {"\tldaxp w0, w1, [x17]"}},
    {"t stlex", 1, T(0xe8c2, 0x1fe3), {"\tstlxr w3, w1, [x17]"}},
    {"t stlexb", 1, T(0xe8c2, 0x1fc3), {"\tstlxrb w3, w1, [x17]"}},
    {"t stlexh", 1, T(0xe8c2, 0x1fd3), {"\tstlxrh w3, w1, [x17]"}},
    {"t stlexd", 1, T(0xe8c2, 0x01f3), {"\tstlxp w3, w0, w1, [x17]"}},
    {"t lda", 1, T(0xe8d2, 0x1faf), {"\tldar w1, [x17]"}},
    {"t ldab", 1, T(0xe8d2, 0x1f8f), {"\tldarb w1, [x17]"}},
    {"t ldah", 1, T(0xe8d2, 0x1f9f), {"\tldarh w1, [x17]"}},
    {"t stl", 1, T(0xe8c2, 0x1faf), {"\tstlr w1, [x17]"}},
    {"t stlb", 1, T(0xe8c2, 0x1f8f), {"\tstlrb w1, [x17]"}},
    {"t stlh", 1, T(0xe8c2, 0x1f9f), {"\tstlrh w1, [x17]"}},
};
#define NCASES (sizeof CASES / sizeof *CASES)

/* plain (unordered) host exclusives, and the one-sided barriers the old lda / stl lifting used: never emitted for these */
static const char *const PLAIN[] = {"\tldxr", "\tldxp", "\tstxr", "\tstxp", "\tdmb"};

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

int main(void) {
    uint8_t *win = mmap(NULL, (size_t)CODE_BASE + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (win == MAP_FAILED) { perror("mmap"); return 1; }
    char dir[] = "/tmp/a32jit_ex_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    int pass = 0, fail = 0;
    for (unsigned k = 0; k < NCASES; k++) {
        const Case *c = &CASES[k];
        uint8_t *p = win + CODE_BASE;
        memset(p, 0, 64);
        if (c->thumb) {
            uint16_t h[4] = {(uint16_t)c->w, (uint16_t)(c->w >> 16), 0xbf00, 0xbf00}; /* + nops */
            memcpy(p, h, sizeof h);
        } else {
            uint32_t wd[2] = {c->w, 0xe320f000};
            memcpy(p, wd, sizeof wd);
        }
        uint32_t seed = CODE_BASE | (c->thumb ? 1u : 0u);
        uint8_t fb = 0;
        LiftModule lm = {"test", CODE_BASE, 64, 0, 0};
        LiftInput in = {.mem = win, .mods = &lm, .nmods = 1, .seeds = &seed, .nseeds = 1, .each_entry = 1, .seed_fb = &fb, .fb_base = CODE_BASE, .fb_stride = 64};
        char pre[256];
        snprintf(pre, sizeof pre, "%s/c%u", dir, k);
        LiftStats st;
        char **files;
        int nf = a32lift_run(&in, pre, &files, &st);
        if (nf < 0) { printf("FAIL %-12s lifter error\n", c->name); fail++; continue; }
        size_t tot = 1;
        char *all = calloc(1, 1);
        for (int f = 0; f < nf; f++) {
            char *s = slurp(files[f]);
            if (!s) continue;
            tot += strlen(s);
            all = realloc(all, tot);
            strcat(all, s);
            free(s);
            unlink(files[f]);
        }
        const char *why = NULL;
        if (fb) why = "fell back to the out-of-line semantics";
        for (unsigned w = 0; !why && w < 3 && c->want[w]; w++)
            if (!strstr(all, c->want[w])) why = c->want[w];
        for (unsigned q = 0; !why && q < sizeof PLAIN / sizeof *PLAIN; q++)
            if (strstr(all, PLAIN[q])) why = PLAIN[q];
        if (why) {
            printf("FAIL %-12s %s\n", c->name, why[0] == '\t' ? why + 1 : why);
            fail++;
        } else pass++;
        free(all);
    }
    rmdir(dir);
    printf("excltest: %d/%d passed\n", pass, pass + fail);
    return fail != 0;
}
