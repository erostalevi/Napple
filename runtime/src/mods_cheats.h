/* Cheat engine of the mods runtime (portable C; the Rust twin is crates/napple-mods/src/cheats.rs, the opcode table is in docs/mods/format.md).
 * The build writes <data-dir>/mods/cheats.txt (Atmosphere-style text for the main module's build id); mods_runtime.c runs it once per frame
 * against the guest address space through McMem. Pure logic here: no guest memory, no files. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { MC_MAIN = 0, MC_HEAP = 1, MC_ALIAS = 2, MC_ASLR = 3, MC_ABS = 4 }; /* ABS: an absolute guest address held in a register */
enum { MC_STORE, MC_IF, MC_ELSE, MC_END, MC_LOOP, MC_ENDLOOP, MC_LOADIMM, MC_LOADMEM, MC_STOREREG, MC_ARITH };
enum { MC_GT = 1, MC_GE, MC_LT, MC_LE, MC_EQ, MC_NE };
enum { MC_ADD, MC_SUB, MC_MUL, MC_LSH, MC_RSH };
typedef struct {
    uint8_t type, width, region, reg, cond, aop, inc, rel, has_off, off;
    uint64_t addr, value;
    uint32_t count;
} McInsn;
typedef struct {
    char name[65];
    int master;
    McInsn *ops;
    size_t nops;
    char *unsupported; /* NULL = fully decoded; a cheat with a reason is not run */
} McCheat;
typedef struct ModsCheats {
    McCheat *c;
    size_t n;
} ModsCheats;
typedef struct {
    int (*read)(void *ctx, int region, uint64_t addr, int width, uint64_t *out); /* 1 = ok */
    int (*write)(void *ctx, int region, uint64_t addr, int width, uint64_t value); /* 1 = ok */
} McMem;
typedef struct {
    unsigned stores, failed;
    int aborted; /* step budget exhausted */
} McStats;
#define MC_MAX_STEPS (1u << 20)

/* Decodes words into ops (*ops malloc'd, possibly the decoded prefix); returns 1 when everything decoded, else 0 with the reason in why. */
int mc_decode(const uint32_t *w, size_t n, McInsn **ops, size_t *nops, char *why, size_t cap);
/* Parses a cheat file; NULL with err filled on a syntax error. */
ModsCheats *mods_cheats_parse(const char *text, size_t len, char *err, size_t cap);
void mods_cheats_free(ModsCheats *c);
McStats mc_run(const McInsn *ops, size_t n, const McMem *mem, void *ctx);
/* Runs every fully decoded cheat (masters first, as they come in the file); adds into *total. */
void mods_cheats_run_all(const ModsCheats *c, const McMem *mem, void *ctx, McStats *total);
