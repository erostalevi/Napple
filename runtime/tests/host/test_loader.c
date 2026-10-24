/* Host unit tests: NRO / NRR validation (nro.c), the ldr:ro / ro:1 service (ro.c, included for its static handlers), run-time module
 * loading in module.c (relocation against a static module and a host override, weak imports, slot reuse) and the dispatch index growing
 * and shrinking with the NRO's gates (dispatch.c). The data directory, the static module and the rewritten NRO are synthesized here in
 * the NTB1 layout of docs/runtime-modules.md. */
#include "../../src/ro.c"
#include "host_stubs.h"
#include "../check.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_SIZE 0x14000
#define TEXT_END 0x4000
#define RO_OFF 0x4000
#define DATA_OFF 0x8000
#define KEY_OFF 0xC000
#define PATCH_OFF 0x10000
#define NRO_SIZE 0xC000

typedef struct { const char *name; uint8_t binding, defined; uint64_t value; } TSym;
typedef struct { uint64_t where; int64_t addend; uint32_t kind, symbol; } TRel;
typedef struct { uint64_t ret, original; uint32_t timer, arg; } TGate;
static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void write_module(const char *dir, const char *file, const uint8_t id[32], const TSym *syms, unsigned nsym, const TRel *rels, unsigned nrel, const TGate *gates, unsigned ngate) {
    char strtab[512] = {0}; unsigned strsz = 1, off[16];
    for (unsigned i = 0; i < nsym; i++) { off[i] = syms[i].name ? strsz : 0; if (syms[i].name) { strcpy(strtab + strsz, syms[i].name); strsz += (unsigned)strlen(syms[i].name) + 1; } }
    char p[1024]; snprintf(p, sizeof p, "%s/%s.tables", dir, file);
    FILE *f = fopen(p, "wb");
    fwrite("NTB1", 4, 1, f); put32(f, 1); put32(f, nsym); put32(f, nrel); put32(f, ngate); put32(f, strsz);
    uint64_t h[12] = {FILE_SIZE, TEXT_END, RO_OFF, DATA_OFF, PATCH_OFF, KEY_OFF, 0, 0, 0, 0, 0, 0};
    fwrite(h, 8, 12, f); fwrite(id, 32, 1, f);
    for (unsigned i = 0; i < nsym; i++) { put32(f, off[i]); uint8_t b[4] = {syms[i].binding, 2, syms[i].defined, 0}; fwrite(b, 4, 1, f); put64(f, syms[i].value); put64(f, 8); }
    for (unsigned i = 0; i < nrel; i++) { put64(f, rels[i].where); put64(f, (uint64_t)rels[i].addend); put32(f, rels[i].kind); put32(f, rels[i].symbol); }
    for (unsigned i = 0; i < ngate; i++) { put64(f, gates[i].ret); put64(f, gates[i].original); put32(f, gates[i].timer); put32(f, gates[i].arg); }
    fwrite(strtab, 1, strsz, f); fclose(f);
    snprintf(p, sizeof p, "%s/%s.patched.bin", dir, file);
    f = fopen(p, "wb"); static uint8_t zero[FILE_SIZE]; fwrite(zero, 1, FILE_SIZE, f); fclose(f);
}
/* The NRO file as the game would read it from its RomFS. */
static uint8_t *make_nro(const uint8_t id[32]) {
    uint8_t *n = aligned_alloc(0x1000, NRO_SIZE); memset(n, 0, NRO_SIZE);
    memcpy(n + 0x10, "NRO0", 4);
    uint32_t v[] = {NRO_SIZE, 0, 0, 0x4000, 0x4000, 0x4000, 0x8000, 0x4000, 0};
    memcpy(n + 0x18, &v[0], 4); memcpy(n + 0x20, &v[2], 4 * 7);
    memcpy(n + 0x40, id, 32);
    for (unsigned i = 0x100; i < 0x200; i++) n[i] = (uint8_t)(i * 7 + id[0]);   /* some code bytes so the hash depends on the content */
    return n;
}
static uint8_t *make_nrr(uint64_t program_id, uint8_t kind, const uint8_t (*hashes)[32], unsigned n) {
    uint8_t *r = aligned_alloc(0x1000, 0x1000); memset(r, 0, 0x1000);
    memcpy(r, "NRR0", 4);
    uint64_t mask = ~0ull; memcpy(r + 0x10, &mask, 8); memcpy(r + 0x18, &program_id, 8);
    memcpy(r + 0x330, &program_id, 8); uint32_t size = 0x1000, off = 0x350; memcpy(r + 0x338, &size, 4); r[0x33c] = kind;
    memcpy(r + 0x340, &off, 4); memcpy(r + 0x344, &n, 4);
    for (unsigned i = 0; i < n; i++) memcpy(r + off + 32 * i, hashes[i], 32);
    return r;
}

static uint64_t args[8]; static IpcCall call;
static uint32_t ro_call(IpcObject *o, unsigned cmd, unsigned nargs) {
    memset(&call, 0, sizeof call);
    call.object = o; call.cmd = cmd; call.in = (const uint8_t *)args; call.in_size = nargs * 8;
    for (size_t i = 0; i < o->ncmds; i++) if (o->cmds[i].cmd == cmd) o->cmds[i].fn(o, &call);
    return call.result;
}
static uint32_t map_nro(IpcObject *o, const uint8_t *nro, uint64_t size, uint64_t bss, uint64_t *out) {
    args[0] = 0; args[1] = (uintptr_t)nro; args[2] = size; args[3] = 0x40000000; args[4] = bss;
    uint32_t r = ro_call(o, 0, 5);
    if (!r && out) memcpy(out, call.out, 8);
    return r;
}

void dispatch_build_index(void);
static int g_test_svc_calls;
static void test_svc(uint64_t *r) { g_test_svc_calls++; r[0] = 0; }
static uint64_t host_fn_target(void) { return 42; }

int main(void) {
    char root[] = "/tmp/napple-loader-XXXXXX"; if (!mkdtemp(root)) return 2;
    char d[1100]; snprintf(d, sizeof d, "%s/modules", root); mkdir(d, 0755);
    char n[1100]; snprintf(n, sizeof n, "%s/nro", root); mkdir(n, 0755);
    char mf[1100]; snprintf(mf, sizeof mf, "%s/manifest.json", root);
    FILE *f = fopen(mf, "w"); fputs("{\"load_order\":[\"main\"],\"modules\":[{\"name\":\"main\"}]}", f); fclose(f);
    g_cfg.data_dir = root; g_program_id = 0x0100000000010000ull;

    static const uint8_t main_id[32] = {0xaa};
    TSym msyms[] = {{NULL, 0, 0, 0}, {"main_sym", 1, 1, 0x8020}};
    write_module(d, "main", main_id, msyms, 2, NULL, 0, NULL, 0);
    static const HostSymbol host[] = {{"host_fn", (void *)host_fn_target}};
    host_symbols_register(host, 1);
    modules_load(root);
    dispatch_build_index();
    const Module *mainm = module_by_name("main");
    CHECK(mainm && module_find_symbol("main_sym") == mainm->base + 0x8020, "static module loaded, symbol exported");

    uint8_t id[32]; for (int i = 0; i < 32; i++) id[i] = (uint8_t)(0x40 + i);
    char idhex[65]; nro_id_hex(id, 32, idhex);
    TSym nsyms[] = {{NULL, 0, 0, 0}, {"main_sym", 1, 0, 0}, {"host_fn", 1, 0, 0}, {"weak_missing", 2, 0, 0}, {"nro_sym", 1, 1, 0x8100}};
    TRel nrels[] = {{0x8000, 0x100, 1027, 0}, {0x8008, 8, 257, 1}, {0x8010, 0, 1025, 2}, {0x8018, 0, 1026, 3}};
    TGate ngates[] = {{0x10004, 0x100, 0, 0x7E}, {0x10010, 0x104, 2, 5}};
    write_module(n, idhex, id, nsyms, 5, nrels, 4, ngates, 2);

    /* NRO / NRR checks */
    uint8_t *nro = make_nro(id);
    NroInfo info;
    CHECK(nro_validate(nro, NRO_SIZE, 0, &info) == 0 && !memcmp(info.module_id, id, 32) && info.data_off == 0x8000, "valid NRO header");
    CHECK(nro_validate(nro, NRO_SIZE, 0x1000, &info) == R_RO_INVALID_NRO, "bss size differs from the header");
    CHECK(nro_validate(nro, NRO_SIZE - 0x1000, 0, &info) == R_RO_INVALID_NRO, "size differs from the header");
    nro[0x2c] = 0x10;   /* ro size 0x4010: not page aligned */
    CHECK(nro_validate(nro, NRO_SIZE, 0, &info) == R_RO_INVALID_NRO, "unaligned section size");
    nro[0x2c] = 0;
    uint8_t hash[32]; sha256(nro, NRO_SIZE, hash);
    uint8_t hashes[3][32]; memset(hashes[0], 0x01, 32); memcpy(hashes[1], hash, 32); memset(hashes[2], 0xff, 32);
    if (memcmp(hashes[1], hashes[0], 32) < 0) memset(hashes[0], 0x00, 32);
    uint8_t *nrr = make_nrr(g_program_id, NRR_KIND_USER, hashes, 3);
    NrrInfo ni;
    CHECK(nrr_validate(nrr, 0x1000, g_program_id, NRR_KIND_USER, &ni) == 0 && nrr_has_hash(nrr, &ni, hash), "NRR valid, hash found by binary search");
    CHECK(nrr_validate(nrr, 0x1000, g_program_id + 1, NRR_KIND_USER, &ni) == R_RO_INVALID_NRR, "NRR of another program");
    CHECK(nrr_validate(nrr, 0x1000, g_program_id, NRR_KIND_JIT_PLUGIN, &ni) == R_RO_INVALID_NRR_KIND, "NRR kind checked");
    CHECK(nrr_validate(nrr, 0x2000, g_program_id, NRR_KIND_USER, &ni) == R_RO_INVALID_SIZE, "NRR size must match its header");
    { uint8_t sha_abc[32]; static const uint8_t want[4] = {0xba, 0x78, 0x16, 0xbf}; sha256("abc", 3, sha_abc); CHECK(!memcmp(sha_abc, want, 4), "sha256 test vector"); }

    /* the service */
    IpcObject *ro = ldr_ro_open();
    uint64_t base = 0;
    CHECK(map_nro(ro, nro, NRO_SIZE, 0, &base) == R_RO_INVALID_SESSION, "map before RegisterProcessHandle");
    args[0] = 0; CHECK(ro_call(ro, 4, 1) == 0, "RegisterProcessHandle");
    CHECK(map_nro(ro, nro, NRO_SIZE, 0, &base) == R_RO_NOT_AUTHORIZED, "NRO not listed in any registered NRR");
    args[0] = 0; args[1] = (uintptr_t)nrr + 8; args[2] = 0x1000;
    CHECK(ro_call(ro, 2, 3) == R_RO_INVALID_ADDRESS, "unaligned NRR address");
    args[1] = (uintptr_t)nrr; CHECK(ro_call(ro, 2, 3) == 0, "RegisterModuleInfo");
    CHECK(map_nro(ro, nro + 0x10, NRO_SIZE, 0, &base) == R_RO_INVALID_ADDRESS, "unaligned NRO address");
    CHECK(map_nro(ro, nro, NRO_SIZE, 0x1000, &base) == R_RO_INVALID_NRO, "bss size mismatch");
    CHECK(map_nro(ro, nro, NRO_SIZE, 0, &base) == 0 && base, "MapManualLoadModuleMemory");
    const Module *m = module_for_address(base);
    CHECK(m && m->dynamic && (uintptr_t)m->base == base, "NRO registered as a module");
    uint64_t v[4]; memcpy(v, (void *)(base + 0x8000), sizeof v);
    CHECK(v[0] == base + 0x100, "RELATIVE relocation");
    CHECK(v[1] == (uintptr_t)mainm->base + 0x8020 + 8, "ABS64 against the static module, with addend");
    CHECK(v[2] == (uintptr_t)host_fn_target, "GLOB_DAT bound to the host override");
    CHECK(v[3] == 0, "unresolved weak import is null");
    CHECK(module_find_symbol("nro_sym") == (void *)(base + 0x8100), "NRO exports become visible");
    uint64_t key; memcpy(&key, (void *)(base + KEY_OFF + 16), 8);
    CHECK(key == (uint64_t)runtime_keys[2] * 8, "key page filled");
    CHECK(map_nro(ro, nro, NRO_SIZE, 0, NULL) == R_RO_ALREADY_LOADED, "same module twice");

    /* the NRO's gates are dispatched */
    svc_register(0x7E, "TestSvc", test_svc);
    uint64_t regs[101] = {0};
    host_thread_attach(calloc(1, 0x200));
    runtime_dispatch(base + 0x10004, regs);
    CHECK(g_test_svc_calls == 1, "svc gate of the NRO reaches its handler");
    runtime_dispatch(base + 0x10010, regs);
    CHECK(regs[5] == 19200000, "cntfrq gate of the NRO");

    /* unload, then load again (slot reuse) */
    args[0] = 0; args[1] = base + 0x1000; CHECK(ro_call(ro, 1, 2) == R_RO_NOT_LOADED, "unmap at the wrong address");
    args[1] = base; CHECK(ro_call(ro, 1, 2) == 0, "UnmapManualLoadModuleMemory");
    CHECK(!module_for_address(base) && !module_find_symbol("nro_sym"), "module gone after unmap");
    args[1] = base; CHECK(ro_call(ro, 1, 2) == R_RO_NOT_LOADED, "unmap twice");
    unsigned before = g_nmodules;
    CHECK(map_nro(ro, nro, NRO_SIZE, 0, &base) == 0 && g_nmodules == before, "reload reuses the module slot");
    runtime_dispatch(base + 0x10004, regs);
    CHECK(g_test_svc_calls == 2, "gates of the reloaded NRO");

    /* an authorized NRO without an ahead-of-time image: refused (lenient mode) */
    uint8_t id2[32]; memset(id2, 0x77, 32);
    uint8_t *nro2 = make_nro(id2); uint8_t h2[32]; sha256(nro2, NRO_SIZE, h2);
    uint8_t *nrr2 = make_nrr(g_program_id, NRR_KIND_USER, (const uint8_t (*)[32])h2, 1);
    args[0] = 0; args[1] = (uintptr_t)nrr2; args[2] = 0x1000; CHECK(ro_call(ro, 2, 3) == 0, "second NRR");
    setenv("NAPPLE_IPC_LENIENT", "1", 1);
    CHECK(map_nro(ro, nro2, NRO_SIZE, 0, NULL) == R_RO_INVALID_NRO, "NRO without a rewritten image");
    unsetenv("NAPPLE_IPC_LENIENT");
    args[1] = (uintptr_t)nrr2; CHECK(ro_call(ro, 3, 2) == 0, "UnregisterModuleInfo");
    CHECK(ro_call(ro, 3, 2) == R_RO_NOT_REGISTERED, "unregister twice");

    /* ro:1 takes JIT plugin NRRs only */
    IpcObject *ro1 = ro1_open();
    args[0] = 0; ro_call(ro1, 4, 1);
    args[0] = 0; args[1] = (uintptr_t)nrr; args[2] = 0x1000;
    CHECK(ro_call(ro1, 2, 3) == R_RO_INVALID_NRR_KIND, "ro:1 refuses a User NRR");
    ro_destroy(ro1); free(ro1);

    /* closing the session unloads its modules */
    ro_destroy(ro); free(ro);
    CHECK(!module_for_address(base), "session close unloads the NRO");
    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); if (system(cmd)) printf("cleanup of %s failed\n", root);
    printf("{\"test\":\"loader\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
