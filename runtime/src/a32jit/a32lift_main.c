/* a32lift: command line tool. Maps the AArch32 modules of a data directory exactly like the runtime (a32_module.c: same bases, same
 * relocations), collects the entry points and writes the AArch64 assembly of the lifted code.
 *   a32lift <data dir>/modules <out prefix> [module ...]      (default module order = the runtime's) */
#include "a32lift.h"
#include "../a32/a32_module.h"
#include "../a32/a32_priv.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const module_order[] = {"main",    "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
                                           "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};

/* Lifts every AArch32 module of `dir` (a game data directory's modules/); writes <prefix>.*.S, <prefix>.files (the list) and <prefix>.manifest.
 * Returns 0 on success. Also the entry point of `napple-rewrite a32` (crates/napple-rewrite/src/a32.rs). */
int a32lift_tool(const char *dir, const char *prefix) {
    A32Window w;
    char err[256];
    if (a32_window_init(&w)) {
        perror("window");
        return 1;
    }
    for (unsigned i = 0; i < sizeof module_order / sizeof *module_order; i++) {
        char p[4200];
        snprintf(p, sizeof p, "%s/%s.tables", dir, module_order[i]);
        if (access(p, R_OK)) continue;
        if (a32_module_map(&w, dir, module_order[i], err, sizeof err)) {
            fprintf(stderr, "%s: %s\n", module_order[i], err);
            return 1;
        }
    }
    A32RelStats rs;
    if (a32_relocate(&w, &rs, err, sizeof err)) {
        fprintf(stderr, "relocate: %s\n", err);
        return 1;
    }
    LiftInput in;
    memset(&in, 0, sizeof in);
    in.mem = w.host;
    in.nmods = w.nmods;
    in.mods = calloc(w.nmods, sizeof *in.mods);
    uint32_t *seeds = NULL;
    size_t ns = 0, cap = 0;
#define SEED(v)                                                                                                                                                \
    do {                                                                                                                                                       \
        if (ns == cap) {                                                                                                                                       \
            cap = cap ? cap * 2 : 4096;                                                                                                                        \
            seeds = realloc(seeds, cap * sizeof *seeds);                                                                                                       \
        }                                                                                                                                                      \
        seeds[ns++] = (v);                                                                                                                                     \
    } while (0)
    char manifest[1100];
    snprintf(manifest, sizeof manifest, "%s.manifest", prefix);
    FILE *mf = fopen(manifest, "w");
    for (unsigned m = 0; m < w.nmods; m++) {
        const A32Module *mod = &w.mods[m];
        in.mods[m].name = mod->name;
        in.mods[m].base = mod->base;
        in.mods[m].text_end = mod->text_end;
        if (getenv("A32LIFT_ONLY")) { /* debugging: lift only the named modules (comma separated); the rest is interpreted */
            char pat[64];
            snprintf(pat, sizeof pat, ",%s,", mod->name);
            char all[512];
            snprintf(all, sizeof all, ",%s,", getenv("A32LIFT_ONLY"));
            if (!strstr(all, pat)) in.mods[m].text_end = 0;
        }
        if (mf) fprintf(mf, "%s %08x %08x %08x\n", mod->name, mod->base, mod->size, mod->text_end);
    }
    if (mf) fclose(mf);
    for (unsigned m = 0; m < w.nmods; m++) {
        const A32Module *mod = &w.mods[m];
        for (uint32_t i = 1; i < mod->nsym; i++) {
            const A32Sym *s = &mod->syms[i];
            if (s->defined && (s->value & ~1ull) < mod->text_end && s->type == 2) SEED(mod->base + (uint32_t)s->value);
        }
        if (mod->dt_init) SEED(mod->base + mod->dt_init);
        for (uint32_t i = 0; i < mod->nrel; i++) { /* relocated slots that point into code: function pointers, vtables, init arrays, GOT */
            uint32_t v;
            memcpy(&v, w.host + mod->base + mod->rels[i].where, 4);
            for (unsigned k = 0; k < w.nmods; k++) {
                const A32Module *t = &w.mods[k];
                uint32_t a = v & ~1u;
                if (a >= t->base && a - t->base < t->text_end) {
                    if ((v & 1) || !(v & 3)) SEED(v);
                    break;
                }
            }
        }
    }
    /* A32LIFT_SEEDS=<file>: extra entry points, one hex guest address per line (bit 0 = Thumb): what a run of the game reached through computed jumps
     * the static walk could not follow (the runtime logs them with NAPPLE_A32_JITLOG=<file>). */
    char seedpath[4200];
    snprintf(seedpath, sizeof seedpath, "%s/a32jit.seeds", dir); /* default: next to the modules */
    const char *seedfile = getenv("A32LIFT_SEEDS") ? getenv("A32LIFT_SEEDS") : (access(seedpath, R_OK) == 0 ? seedpath : NULL);
    if (seedfile) {
        FILE *sf = fopen(seedfile, "r");
        unsigned v;
        unsigned extra = 0;
        while (sf && fscanf(sf, "%x", &v) == 1) {
            SEED(v);
            extra++;
        }
        if (sf) fclose(sf);
        fprintf(stderr, "a32lift: %u extra seeds from %s\n", extra, seedfile);
    }
    in.seeds = seeds;
    in.nseeds = (unsigned)ns;
    LiftStats st;
    char **files;
    char covpath[1100];
    snprintf(covpath, sizeof covpath, "%s.cov", prefix);
    in.cov = fopen(covpath, "w");
    int n = a32lift_run(&in, prefix, &files, &st);
    if (in.cov) {
        fprintf(in.cov, "#stats %llu %llu\n", (unsigned long long)st.lifted, (unsigned long long)st.fallback);
        fclose(in.cov);
    }
    if (n < 0) return 1;
    fprintf(stderr, "a32lift: %u modules, %zu seeds, %llu entries, %llu insns visited, %llu emitted (%llu native, %llu via interpreter), %d files\n", w.nmods,
            ns, (unsigned long long)st.entries, (unsigned long long)st.insns, (unsigned long long)(st.lifted + st.fallback), (unsigned long long)st.lifted,
            (unsigned long long)st.fallback, n);
    for (unsigned op = 0; op < 64; op++)
        if (st.fb_by_op[op]) fprintf(stderr, "  fallback op %-3u %llu\n", op, (unsigned long long)st.fb_by_op[op]);
    FILE *lf = fopen((snprintf(manifest, sizeof manifest, "%s.files", prefix), manifest), "w");
    if (lf) {
        for (int i = 0; i < n; i++) fprintf(lf, "%s\n", files[i]);
        fclose(lf);
    }
    for (int i = 0; i < n; i++) free(files[i]);
    free(files);
    free(seeds);
    free(in.mods);
    a32_window_free(&w);
    return 0;
}

#ifndef A32LIFT_NO_MAIN
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: a32lift <modules dir> <out prefix>\n");
        return 2;
    }
    return a32lift_tool(argv[1], argv[2]);
}
#endif
