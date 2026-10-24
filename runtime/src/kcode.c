/* Code memory (svc 0x4B CreateCodeMemory, 0x4C ControlCodeMemory): the JIT interface. A code memory wraps a range of the guest heap; Map
 * gives the process a read-write alias of it (memory state CodeOut), MapToOwner a read-only or read-execute alias (GeneratedCode).
 * Argument and state checks follow the public Horizon kernel (KCodeMemory) and are pure; the aliasing itself is memory.c's
 * (memory_code_alias / memory_code_unalias, mach_vm_remap of the source pages, so writes through either alias are shared).
 *
 * Limits: the alias addresses must be host-page (16 KiB) aligned or the mapping fails with InvalidAddress (a copy would break the sharing
 * a JIT relies on), and code generated at run time executes natively without the AOT rewrite, so it must not contain svc, x18 or
 * tpidr/tpidrro accesses. */
#include "napple.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { CODE_MAP = 0, CODE_MAP_TO_OWNER = 1, CODE_UNMAP = 2, CODE_UNMAP_FROM_OWNER = 3 };
enum { PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
#define STATE_GENERATED_CODE 0x14
#define STATE_CODE_OUT 0x15

typedef struct KCodeMemory { uint64_t src, size, rw_addr, owner_addr; int rw_mapped, owner_mapped; unsigned owner_perm; struct KCodeMemory *next; } KCodeMemory;
static KCodeMemory *g_code;
static pthread_mutex_t g_code_lock = PTHREAD_MUTEX_INITIALIZER;

/* memory.c (host side; replaced by fakes in runtime/tests/host) */
int memory_is_heap(uint64_t addr, uint64_t size);            /* committed guest heap */
int memory_range_unused(uint64_t addr, uint64_t size);       /* inside the guest address space and mapped by nothing */
uint32_t memory_code_alias(uint64_t dst, uint64_t src, uint64_t size, unsigned perm);
void memory_code_unalias(uint64_t dst, uint64_t size);

static int aligned(uint64_t v) { return (v & 0xfff) == 0; }
static int overlaps_alias(uint64_t a, uint64_t n) {
    for (KCodeMemory *c = g_code; c; c = c->next) {
        if (c->rw_mapped && a < c->rw_addr + c->size && c->rw_addr < a + n) return 1;
        if (c->owner_mapped && a < c->owner_addr + c->size && c->owner_addr < a + n) return 1;
    }
    return 0;
}

/* CreateCodeMemory(x1 address, x2 size) -> w1 handle */
uint32_t kcode_create(uint64_t addr, uint64_t size, uint32_t *out) {
    if (!aligned(addr)) return R_INVALID_ADDRESS;
    if (!size || !aligned(size)) return R_INVALID_SIZE;
    if (addr + size <= addr) return R_INVALID_CURRENT_MEMORY;
    if (!memory_is_heap(addr, size)) return R_INVALID_CURRENT_MEMORY;
    KCodeMemory *c = calloc(1, sizeof *c);
    if (!c) return R_OUT_OF_RESOURCE;
    c->src = addr; c->size = size;
    uint32_t h = handle_new(OBJ_CODEMEM, c);
    if (!h) { free(c); return R_OUT_OF_RESOURCE; }
    pthread_mutex_lock(&g_code_lock); c->next = g_code; g_code = c; pthread_mutex_unlock(&g_code_lock);
    *out = h;
    return 0;
}

/* ControlCodeMemory(w0 handle, w1 operation, x2 address, x3 size, w4 permission) */
uint32_t kcode_control(uint32_t h, unsigned op, uint64_t addr, uint64_t size, unsigned perm) {
    KCodeMemory *c = handle_get(h, OBJ_CODEMEM);
    if (!c) return R_INVALID_HANDLE;
    if (!aligned(addr)) return R_INVALID_ADDRESS;
    if (!size || !aligned(size)) return R_INVALID_SIZE;
    if (addr + size <= addr) return R_INVALID_MEMORY_REGION;
    uint32_t res = 0;
    pthread_mutex_lock(&g_code_lock);
    switch (op) {
    case CODE_MAP:
        if (perm != PERM_RW) { res = R_INVALID_MEMORY_PERMISSIONS; break; }
        if (size != c->size) { res = R_INVALID_SIZE; break; }
        if (c->rw_mapped) { res = R_INVALID_STATE; break; }
        if (!memory_range_unused(addr, size) || overlaps_alias(addr, size)) { res = R_INVALID_MEMORY_REGION; break; }
        if (!(res = memory_code_alias(addr, c->src, size, PERM_RW))) { c->rw_addr = addr; c->rw_mapped = 1; }
        break;
    case CODE_MAP_TO_OWNER:
        if (perm != PERM_R && perm != PERM_RX) { res = R_INVALID_MEMORY_PERMISSIONS; break; }
        if (size != c->size) { res = R_INVALID_SIZE; break; }
        if (c->owner_mapped) { res = R_INVALID_STATE; break; }
        if (!memory_range_unused(addr, size) || overlaps_alias(addr, size)) { res = R_INVALID_MEMORY_REGION; break; }
        if (!(res = memory_code_alias(addr, c->src, size, perm))) { c->owner_addr = addr; c->owner_mapped = 1; c->owner_perm = perm; }
        break;
    case CODE_UNMAP:
        if (size != c->size) { res = R_INVALID_SIZE; break; }
        if (!c->rw_mapped || c->rw_addr != addr) { res = R_INVALID_MEMORY_REGION; break; }
        memory_code_unalias(addr, size); c->rw_mapped = 0;
        break;
    case CODE_UNMAP_FROM_OWNER:
        if (size != c->size) { res = R_INVALID_SIZE; break; }
        if (!c->owner_mapped || c->owner_addr != addr) { res = R_INVALID_MEMORY_REGION; break; }
        memory_code_unalias(addr, size); c->owner_mapped = 0;
        break;
    default: res = R_INVALID_ENUM;
    }
    pthread_mutex_unlock(&g_code_lock);
    return res;
}

/* QueryMemory over the aliases: 1 when addr lies in one. */
int kcode_query(uint64_t addr, uint64_t *base, uint64_t *size, unsigned *state, unsigned *perm) {
    int found = 0;
    pthread_mutex_lock(&g_code_lock);
    for (KCodeMemory *c = g_code; c && !found; c = c->next) {
        if (c->rw_mapped && addr >= c->rw_addr && addr < c->rw_addr + c->size) { *base = c->rw_addr; *size = c->size; *state = STATE_CODE_OUT; *perm = PERM_RW; found = 1; }
        else if (c->owner_mapped && addr >= c->owner_addr && addr < c->owner_addr + c->size) { *base = c->owner_addr; *size = c->size; *state = STATE_GENERATED_CODE; *perm = c->owner_perm; found = 1; }
    }
    pthread_mutex_unlock(&g_code_lock);
    return found;
}

/* CloseHandle: the aliases go away with the object (the kernel refuses to destroy a mapped code memory; the SDK unmaps first). */
int kcode_close_object(ObjType type, void *obj) {
    if (type != OBJ_CODEMEM) return 0;
    KCodeMemory *c = obj;
    pthread_mutex_lock(&g_code_lock);
    for (KCodeMemory **p = &g_code; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    pthread_mutex_unlock(&g_code_lock);
    if (c->rw_mapped) memory_code_unalias(c->rw_addr, c->size);
    if (c->owner_mapped) memory_code_unalias(c->owner_addr, c->size);
    free(c);
    return 1;
}

static void svc_create_code_memory(uint64_t *r) { uint32_t h = 0; r[0] = kcode_create(r[1], r[2], &h); r[1] = h; }
static void svc_control_code_memory(uint64_t *r) { r[0] = kcode_control((uint32_t)r[0], (unsigned)r[1], r[2], r[3], (unsigned)r[4]); }
void svc_kcode_register(void) {
    svc_register(0x4B, "CreateCodeMemory", svc_create_code_memory);
    svc_register(0x4C, "ControlCodeMemory", svc_control_code_memory);
}
