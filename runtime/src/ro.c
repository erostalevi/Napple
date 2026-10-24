/* ldr:ro and ro:1: run-time loading of NRO modules (nn::ro::LoadModule). The guest reads an NRO into its own memory, registers the NRR
 * that lists the hashes of the NROs it may load, and asks the service to map the module; nn::ro then links it in user mode.
 *   0 MapManualLoadModuleMemory(pid, u64 nro_address, u64 nro_size, u64 bss_address, u64 bss_size) -> u64 address
 *   1 UnmapManualLoadModuleMemory(pid, u64 address)     2 RegisterModuleInfo(pid, u64 nrr_address, u64 nrr_size)
 *   3 UnregisterModuleInfo(pid, u64 nrr_address)          4 RegisterProcessHandle(pid, copy handle process)
 *   10 RegisterProcessModuleInfo(pid, u64 nrr_address, u64 nrr_size, copy handle process)
 * Checks are the ro system module's (nro.c). The NRO bytes the guest passes are only validated and hashed: what gets mapped is the image
 * `napple build` rewrote ahead of time from the same file, found by module id (module_load_nro). ldr:ro takes NRRs of kind User, ro:1
 * those of kind JitPlugin. An NRO without an ahead-of-time image stops the runtime (NAPPLE_IPC_LENIENT=1: InvalidNro). */
#include "ipc.h"
#include "nro.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

#define RO_MAX_NRR 0x40
#define RO_MAX_NRO 0x40
typedef struct { uint64_t address; uint8_t *copy; NrrInfo info; } RoNrr;
typedef struct { uint64_t address, nro_address, nro_size, bss_address, bss_size; uint8_t module_id[32]; Module *module; } RoNro;
typedef struct { int kind, registered; RoNrr nrr[RO_MAX_NRR]; RoNro nro[RO_MAX_NRO]; } RoContext;

static int aligned(uint64_t v) { return (v & 0xfff) == 0; }

static uint32_t register_nrr(RoContext *x, uint64_t addr, uint64_t size) {
    if (!x->registered) return R_RO_INVALID_SESSION;
    if (!aligned(addr)) return R_RO_INVALID_ADDRESS;
    if (!size || !aligned(size)) return R_RO_INVALID_SIZE;
    const uint8_t *p = guest_ptr(addr, size);
    if (!p) return R_RO_INVALID_ADDRESS;
    RoNrr *slot = NULL;
    for (unsigned i = 0; i < RO_MAX_NRR; i++) if (!x->nrr[i].copy) { slot = &x->nrr[i]; break; }
    if (!slot) return R_RO_TOO_MANY_NRR;
    NrrInfo info;
    uint32_t r = nrr_validate(p, size, g_program_id, x->kind, &info);
    if (r) { LOG("ro", "NRR at %#llx rejected (%#x)", (unsigned long long)addr, r); return r; }
    slot->copy = malloc(size);                       /* the console makes the NRR inaccessible to the process; a copy keeps it unchanged here */
    if (!slot->copy) return R_RO_INTERNAL_ERROR;
    memcpy(slot->copy, p, size); slot->address = addr; slot->info = info;
    LOGV("ro", "NRR %#llx registered: %u hashes", (unsigned long long)addr, info.nhashes);
    return 0;
}
static uint32_t unregister_nrr(RoContext *x, uint64_t addr) {
    if (!x->registered) return R_RO_INVALID_SESSION;
    if (!aligned(addr)) return R_RO_INVALID_ADDRESS;
    for (unsigned i = 0; i < RO_MAX_NRR; i++) if (x->nrr[i].copy && x->nrr[i].address == addr) { free(x->nrr[i].copy); x->nrr[i] = (RoNrr){0}; return 0; }
    return R_RO_NOT_REGISTERED;
}
static int authorized(const RoContext *x, const uint8_t hash[32]) {
    for (unsigned i = 0; i < RO_MAX_NRR; i++) if (x->nrr[i].copy && nrr_has_hash(x->nrr[i].copy, &x->nrr[i].info, hash)) return 1;
    return 0;
}

/* Everything up to the mapping: on success *slot_out is the free NRO slot and info the parsed header. Pure apart from guest_ptr (tested). */
static uint32_t ro_check_map(RoContext *x, uint64_t nro_addr, uint64_t nro_size, uint64_t bss_addr, uint64_t bss_size, NroInfo *info, RoNro **slot_out) {
    if (!x->registered) return R_RO_INVALID_SESSION;
    if (!aligned(nro_addr)) return R_RO_INVALID_ADDRESS;
    if (!nro_size || !aligned(nro_size)) return R_RO_INVALID_SIZE;
    if (!aligned(bss_addr)) return R_RO_INVALID_ADDRESS;
    if (!aligned(bss_size)) return R_RO_INVALID_SIZE;
    if (nro_size + bss_size < nro_size || nro_addr + nro_size < nro_addr) return R_RO_INVALID_SIZE;
    RoNro *slot = NULL;
    for (unsigned i = 0; i < RO_MAX_NRO; i++) if (!x->nro[i].address) { slot = &x->nro[i]; break; }
    if (!slot) return R_RO_TOO_MANY_NRO;
    const uint8_t *img = guest_ptr(nro_addr, nro_size);
    if (!img) return R_RO_INVALID_ADDRESS;
    uint32_t r = nro_validate(img, nro_size, bss_size, info);
    if (r) return r;
    uint8_t hash[32]; sha256(img, nro_size, hash);
    if (!authorized(x, hash)) return R_RO_NOT_AUTHORIZED;
    for (unsigned i = 0; i < RO_MAX_NRO; i++) if (x->nro[i].address && !memcmp(x->nro[i].module_id, info->module_id, 32)) return R_RO_ALREADY_LOADED;
    *slot_out = slot;
    return 0;
}

/* ---- IPC ------------------------------------------------------------------------------------------------------------- */
static void ro_map(IpcObject *self, IpcCall *c) {
    RoContext *x = self->state;
    uint64_t nro_addr = IPC_IN(c, uint64_t, 8), nro_size = IPC_IN(c, uint64_t, 16), bss_addr = IPC_IN(c, uint64_t, 24), bss_size = IPC_IN(c, uint64_t, 32);
    NroInfo info = {0}; RoNro *slot = NULL;
    uint32_t r = ro_check_map(x, nro_addr, nro_size, bss_addr, bss_size, &info, &slot);
    char id[65]; nro_id_hex(info.module_id, 32, id);
    if (r) { LOG("ro", "MapManualLoadModuleMemory(%#llx+%#llx, bss %#llx) refused: %#x", (unsigned long long)nro_addr, (unsigned long long)nro_size, (unsigned long long)bss_size, r); c->result = r; return; }
    const char *why = NULL;
    Module *m = module_load_nro(g_cfg.data_dir, info.module_id, &why);
    if (!m) {
        LOG("ro", "NRO %s (%#llx bytes) cannot be loaded: %s", id, (unsigned long long)nro_size, why);
        summary_note("NRO %s not loadable: %s", id, why);
        if (!getenv("NAPPLE_IPC_LENIENT")) runtime_fail("NRO %s: %s", id, why);
        c->result = R_RO_INVALID_NRO; return;
    }
    *slot = (RoNro){(uintptr_t)m->base, nro_addr, nro_size, bss_addr, bss_size, {0}, m};
    memcpy(slot->module_id, info.module_id, 32);
    trace_event("nro_load", "\"module_id\":\"%s\",\"address\":\"%p\"", id, (void *)m->base);
    uint64_t out = slot->address; IPC_OUT(c, out);
}
static void ro_unmap(IpcObject *self, IpcCall *c) {
    RoContext *x = self->state; uint64_t addr = IPC_IN(c, uint64_t, 8);
    if (!x->registered) { c->result = R_RO_INVALID_SESSION; return; }
    if (!aligned(addr)) { c->result = R_RO_INVALID_ADDRESS; return; }
    for (unsigned i = 0; i < RO_MAX_NRO; i++) if (x->nro[i].address && x->nro[i].address == addr) {
        module_unload(x->nro[i].module); x->nro[i] = (RoNro){0}; return;
    }
    c->result = R_RO_NOT_LOADED;
}
static void ro_register_nrr(IpcObject *self, IpcCall *c) { uint32_t r = register_nrr(self->state, IPC_IN(c, uint64_t, 8), IPC_IN(c, uint64_t, 16)); if (r) c->result = r; }
static void ro_unregister_nrr(IpcObject *self, IpcCall *c) { uint32_t r = unregister_nrr(self->state, IPC_IN(c, uint64_t, 8)); if (r) c->result = r; }
static void ro_register_process(IpcObject *self, IpcCall *c) { RoContext *x = self->state; (void)c; x->registered = 1; }
static void ro_register_process_nrr(IpcObject *self, IpcCall *c) {
    RoContext *x = self->state; x->registered = 1;
    uint32_t r = register_nrr(x, IPC_IN(c, uint64_t, 8), IPC_IN(c, uint64_t, 16)); if (r) c->result = r;
}
static const IpcCmd ro_cmds[] = {
    {0, "MapManualLoadModuleMemory", ro_map, IPC_IMPL}, {1, "UnmapManualLoadModuleMemory", ro_unmap, IPC_IMPL},
    {2, "RegisterModuleInfo", ro_register_nrr, IPC_IMPL}, {3, "UnregisterModuleInfo", ro_unregister_nrr, IPC_IMPL},
    {4, "RegisterProcessHandle", ro_register_process, IPC_IMPL}, {10, "RegisterProcessModuleInfo", ro_register_process_nrr, IPC_IMPL},
};
/* Closing the session unloads what it loaded, as the system module does when a client goes away. */
static void ro_destroy(IpcObject *o) {
    RoContext *x = o->state;
    for (unsigned i = 0; i < RO_MAX_NRO; i++) if (x->nro[i].address) module_unload(x->nro[i].module);
    for (unsigned i = 0; i < RO_MAX_NRR; i++) free(x->nrr[i].copy);
    free(x);
}
static IpcObject *ro_open_kind(int kind) {
    RoContext *x = calloc(1, sizeof *x);
    if (!x) runtime_fail("ro context");
    x->kind = kind;
    return ipc_object_new(kind == NRR_KIND_USER ? "ldr:ro" : "ro:1", ro_cmds, sizeof ro_cmds / sizeof *ro_cmds, x, ro_destroy);
}
static IpcObject *ldr_ro_open(void) { return ro_open_kind(NRR_KIND_USER); }
static IpcObject *ro1_open(void) { return ro_open_kind(NRR_KIND_JIT_PLUGIN); }
void service_register_ro(void) { ipc_register_service("ldr:ro", ldr_ro_open); ipc_register_service("ro:1", ro1_open); }
