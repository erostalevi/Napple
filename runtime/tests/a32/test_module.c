/* Host test for the AArch32 module loader against a real data directory: test_module <dir with modules/> [list-imports]
 * (e.g. work/games/0100152000022000/<buildid>). Prints module layout, relocation counts and the imports no module defines. */
#include "../../src/a32/a32_module.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <data dir> [list-imports]\n", argv[0]); return 2; }
    char dir[1024], err[256] = "";
    snprintf(dir, sizeof dir, "%s/modules", argv[1]);
    static const char *order[] = { "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "sdk" };
    A32Window w;
    if (a32_window_init(&w)) { perror("window"); return 1; }
    for (unsigned i = 0; i < sizeof order / sizeof *order; i++) {
        if (a32_module_map(&w, dir, order[i], err, sizeof err)) { printf("FAIL map: %s\n", err); return 1; }
        const A32Module *m = &w.mods[w.nmods - 1];
        printf("%-8s guest %#010x size %#09x text_end %#x ro %#x data %#x syms %u rels %u init_array %u\n", m->name, m->base, m->size, m->text_end, m->ro_off, m->data_off, m->nsym, m->nrel, m->init_array_sz / 4);
    }
    A32RelStats st;
    if (a32_relocate(&w, &st, err, sizeof err)) { printf("FAIL relocate: %s\n", err); return 1; }
    printf("relocations applied %llu: RELATIVE %llu ABS32 %llu GLOB_DAT %llu JUMP_SLOT %llu; imports -> module %llu, -> HLE stub %llu; weak undefined %llu; distinct HLE imports %u\n",
           (unsigned long long)w.applied, (unsigned long long)st.relative, (unsigned long long)st.abs32, (unsigned long long)st.glob_dat, (unsigned long long)st.jump_slot,
           (unsigned long long)st.to_module, (unsigned long long)st.to_hle, (unsigned long long)w.weak_unresolved, w.nhle);
    /* sanity: every init_array entry must point into text of some module after relocation */
    unsigned bad = 0, total = 0;
    for (unsigned i = 0; i < w.nmods; i++) {
        const A32Module *m = &w.mods[i];
        for (uint32_t k = 0; k < m->init_array_sz / 4; k++) {
            uint32_t f = *(uint32_t *)(w.host + m->base + m->init_array + 4 * k) & ~1u; total++;
            int ok = 0; for (unsigned j = 0; j < w.nmods; j++) if (f >= w.mods[j].base && f < w.mods[j].base + w.mods[j].text_end) ok = 1;
            bad += !ok;
        }
    }
    printf("init_array entries %u, outside any module text: %u\n", total, bad);
    if (argc > 2 && !strcmp(argv[2], "list-imports")) for (unsigned i = 0; i < w.nhle; i++) printf("  import %s\n", w.hle_names[i]);
    a32_window_free(&w);
    return bad ? 1 : 0;
}
