/* a32_module.h - AArch32 module loader: maps the plain ELF32 images written by `napple build` (crates/napple-build/src/tables32.rs, NTB2 tables)
 * into a 4 GiB guest window, applies their REL relocations and resolves imports across modules and against HLE stubs.
 * Guest addresses are 32-bit; the host address of guest address `a` is `win->host + a`. */
#ifndef A32_MODULE_H
#define A32_MODULE_H
#include <stdint.h>
#include <stddef.h>

#define A32_MAX_MODULES 16
#define A32_HLE_BASE 0xF0000000u /* guest addresses of host-implemented imports: [A32_HLE_BASE, A32_HLE_BASE + 4 * nhle) */

typedef struct {
    uint32_t name;
    uint8_t bind, type, defined, pad;
    uint64_t value, size;
} A32Sym;
typedef struct {
    uint64_t where;
    int64_t addend;
    uint32_t kind, sym;
} A32Rel;

typedef struct {
    char name[16];
    uint32_t base, size; /* guest base and mapped size */
    uint32_t text_end, ro_off, data_off;
    uint32_t dt_init, dt_fini, init_array, init_array_sz, fini_array,
        fini_array_sz; /* module-relative; init/fini arrays hold absolute guest addresses after relocation */
    uint8_t build_id[32];
    A32Sym *syms;
    uint32_t nsym;
    char *strtab;
    uint32_t strtab_len;
    A32Rel *rels;
    uint32_t nrel;
    uint32_t *hash;
    uint32_t hash_mask; /* exported symbol hash (open addressing over symbol indices) */
} A32Module;

typedef struct {
    uint8_t *host; /* host address of guest address 0 (4 GiB reservation) */
    A32Module mods[A32_MAX_MODULES];
    unsigned nmods;
    uint32_t next_base; /* next free guest address for a module */
    char **hle_names;
    unsigned nhle, caphle; /* imports no module defines, in first-use order: stub i lives at A32_HLE_BASE + 4 * i */
    uint64_t applied, unresolved, weak_unresolved;
    int (*is_override)(
        const char *name); /* names the host implements itself (the NVN loader): imports of them resolve to HLE stubs even when a module defines them */
} A32Window;

typedef struct {
    uint64_t relative, abs32, glob_dat, jump_slot, to_module, to_hle;
} A32RelStats;

int a32_window_init(A32Window *w); /* reserves the 4 GiB guest window; 0 on success */
void a32_window_free(A32Window *w);
/* Maps one module (<dir>/<name>.patched.bin + .tables) at the next free guest base. 0 on success, else a message in err. */
int a32_module_map(A32Window *w, const char *dir, const char *name, char *err, size_t errn);
/* Applies the relocations of every mapped module (call after all modules are mapped). */
int a32_relocate(A32Window *w, A32RelStats *st, char *err, size_t errn);
uint32_t a32_find_symbol(const A32Window *w, const char *name);    /* guest address of an exported symbol, 0 if none */
uint32_t a32_hle_stub(A32Window *w, const char *name);             /* guest address of the stub for a host-implemented import (allocated on first use) */
const char *a32_hle_name(const A32Window *w, uint32_t guest_addr); /* the import a stub address stands for, NULL if not a stub */
#endif
