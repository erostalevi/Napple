/* Guest address space and the memory svcs.
 *
 * The SDK encodes IPC buffer addresses with 39 bits, so every byte the guest addresses lies below 2^39. The runtime
 * reserves one contiguous PROT_NONE range there and carves it like Horizon does:
 *
 *      [ unused 256 MiB ][ stack region 256 MiB ][ unused 256 MiB ][ heap region (SetHeapSize, 4 GiB cap) ] ... [ alias region ]
 *
 * (the same range, from its start to the end of the alias region, is the "ASLR region" GetInfo reports.) Modules are mapped
 * by module.c wherever the host allows, thread stacks come from the guest heap through MapMemory, and MapPhysicalMemory
 * commits pages of the alias region. macOS 27 leaves holes of only 7 to 22 GiB below 2^39, so the alias region is sized
 * from what the biggest hole allows (the real 39-bit Horizon size is 64 GiB; games use a small part of it). */
#include "napple.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

int kcode_query(uint64_t addr, uint64_t *base, uint64_t *size, unsigned *state, unsigned *perm);
void svc_kcode_register(void);

#define GUEST_LIMIT (1ULL << 39)
#define STACK_OFF 0x10000000ULL
#define STACK_SIZE 0x10000000ULL
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ULL)
#define HEAP_CAP (4ULL << 30)
#define LOW_SIZE (HEAP_OFF + HEAP_CAP + (1ULL << 30))
#define ALIAS_MAX (64ULL << 30)
#define SEARCH_FROM 0x300000000ULL

uint64_t g_program_id;
uint64_t g_application_pool_bytes = 0xcd500000ULL;

static uint8_t *low_space, *alias_space, *heap;
static uint64_t alias_len, heap_size, alias_bytes;
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t alias_state[ALIAS_MAX / PAGE_4K];    /* per 4 KiB: 0 free, 5 physical memory, 6 shared, 0xd transfer */
static uint8_t alias_stale[ALIAS_MAX / PAGE_4K];    /* unmapped but host page still dirty: zero again when mapped */
static uint8_t exception_context[0x1000] __attribute__((aligned(0x1000)));

typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[4096];
static unsigned nstack_maps;

static uint8_t *reserve_fixed(uint64_t at, uint64_t size) {
    mach_vm_address_t a = at;   /* FIXED without OVERWRITE: fails instead of replacing an existing mapping */
    return mach_vm_map(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT) == KERN_SUCCESS
        ? (uint8_t *)(uintptr_t)a : NULL;
}
static uint64_t alias_requested(void) {
    const char *e = getenv("NAPPLE_ALIAS_GIB");
    uint64_t g = e ? strtoull(e, NULL, 0) : 64;
    if (g < 1 || g > 64) runtime_fail("NAPPLE_ALIAS_GIB must be 1..64");
    return g << 30;
}

void memory_init(void) {
    if (low_space) return;
    pthread_mutex_lock(&mem_lock);
    if (low_space) { pthread_mutex_unlock(&mem_lock); return; }
    /* The preferred fixed layout (alias at 0x70_0000_0000) works on older macOS; otherwise one reservation in a free
     * hole, with the alias region shrinking in 4 GiB steps from the requested size down to 4 GiB. */
    uint64_t want = alias_requested(), least = getenv("NAPPLE_ALIAS_GIB") ? want : (4ULL << 30);
    uint8_t *space = NULL;
    for (uint64_t g = want; !space; g -= 4ULL << 30) {
        for (uint64_t at = SEARCH_FROM; at + LOW_SIZE + g <= GUEST_LIMIT && !space; at += 1ULL << 30)
            if ((space = reserve_fixed(at, LOW_SIZE + g))) alias_len = g;
        if (g < least + (4ULL << 30)) break;
    }
    if (!space) runtime_fail("cannot reserve the guest address space below 2^39 (need %llu GiB)", (unsigned long long)((LOW_SIZE + least) >> 30));
    low_space = space; alias_space = space + LOW_SIZE; heap = space + HEAP_OFF;
    LOG("memory", "guest layout: stack %#llx+%#llx heap %#llx (cap %#llx) alias %#llx+%#llx", (unsigned long long)(uintptr_t)(low_space + STACK_OFF),
        (unsigned long long)STACK_SIZE, (unsigned long long)(uintptr_t)heap, (unsigned long long)HEAP_CAP, (unsigned long long)(uintptr_t)alias_space, (unsigned long long)alias_len);
    pthread_mutex_unlock(&mem_lock);
}

uint64_t memory_heap_base(void) { memory_init(); return (uintptr_t)heap; }
uint64_t memory_heap_cap(void) { return HEAP_CAP; }
uint64_t memory_alias_base(void) { memory_init(); return (uintptr_t)alias_space; }
uint64_t memory_alias_size(void) { memory_init(); return alias_len; }
uint64_t memory_stack_region_base(void) { memory_init(); return (uintptr_t)low_space + STACK_OFF; }
uint64_t memory_stack_region_size(void) { return STACK_SIZE; }
uint64_t memory_aslr_base(void) { memory_init(); return (uintptr_t)low_space; }
uint64_t memory_aslr_size(void) { memory_init(); return LOW_SIZE + alias_len; }
uint64_t memory_used_bytes(void) { return module_image_bytes() + heap_size + alias_bytes; }
uint64_t memory_exception_context(void) { return (uintptr_t)exception_context; }

void *guest_ptr(uint64_t addr, uint64_t size) {
    if (addr < 0x10000 || addr + size < addr || addr + size > GUEST_LIMIT) return NULL;
    return (void *)(uintptr_t)addr;
}
int memory_contains(uint64_t addr, uint64_t size) { return guest_ptr(addr, size) != NULL; }

/* gfx owns write-fault tracking on memory it wrapped; without a gfx the hooks are weak no-ops (gfx_stub.c overrides). */
__attribute__((weak)) void gfx_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }
__attribute__((weak)) int gfx_handle_fault(void *addr, int is_write, void *ctx) { (void)addr; (void)is_write; (void)ctx; return 0; }
void guest_write_prepare(uint64_t addr, size_t size) { gfx_guest_write_prepare((void *)(uintptr_t)addr, size); }
int guest_fault(void *addr, int is_write, void *ctx) { return gfx_handle_fault(addr, is_write, ctx); }

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int in_heap_region(uint64_t a, uint64_t n) { return a >= (uintptr_t)heap && n <= HEAP_CAP && a - (uintptr_t)heap <= HEAP_CAP - n; }
static int in_alias(uint64_t a, uint64_t n) { return a >= (uintptr_t)alias_space && n <= alias_len && a - (uintptr_t)alias_space <= alias_len - n; }

/* ---- MapMemory / UnmapMemory ------------------------------------------------------------------------------------
 * Horizon maps heap or alias pages into the stack region and makes the source inaccessible meanwhile. Each mapping
 * here has its own host backing: the bytes are copied in and the source protected; unmapping copies them back. */
static uint64_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t size) {
    if (!size || ((dst | src | size) & 4095)) return R_INVALID_ADDRESS;
    uint64_t sbase = memory_stack_region_base();
    if (dst < sbase || size > STACK_SIZE || dst - sbase > STACK_SIZE - size) return 0xdc01;   /* InvalidMemoryRegion */
    int heap_src = in_heap_region(src, size) && size <= heap_size && src - (uintptr_t)heap <= heap_size - size;
    int alias_src = in_alias(src, size);
    /* The SDK probes candidate stack addresses by mapping one page of its own thread stack there. */
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self()); size_t stack = pthread_get_stacksize_np(pthread_self());
    int thread_src = src >= top - stack && size <= stack && src + size <= top;
    const Module *msrc = module_for_address(src);
    int module_src = msrc && src + size <= (uintptr_t)msrc->base + msrc->key_off;
    if (!heap_src && !alias_src && !thread_src && !module_src) {
        LOG("memory", "%sMapMemory dst=%#llx src=%#llx size=%#llx: source is not heap, alias, module data or this thread's stack (stack %#llx..%#llx)", unmap ? "Un" : "",
            (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size, (unsigned long long)(top - stack), (unsigned long long)top);
        return 0xdc01;
    }
    pthread_mutex_lock(&mem_lock);
    if (alias_src) {
        unsigned first = (unsigned)((src - (uintptr_t)alias_space) / PAGE_4K);
        for (uint64_t i = 0; i < size / PAGE_4K; i++) if (!alias_state[first + i]) { pthread_mutex_unlock(&mem_lock); return 0xdc01; }
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++) if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == size) found = i;
    uint64_t lo = dst & ~(PAGE_16K - 1), hi = (dst + size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    uint64_t slo = (src + PAGE_16K - 1) & ~(PAGE_16K - 1), shi = (src + size) & ~(PAGE_16K - 1);   /* whole host pages inside the source */
    if (unmap) {
        if (found == nstack_maps) { pthread_mutex_unlock(&mem_lock); return R_INVALID_CURRENT_MEMORY; }
        if (shi > slo && mprotect((void *)slo, shi - slo, PROT_READ | PROT_WRITE)) runtime_fail("restore moved stack source");
        memcpy((void *)src, (const void *)dst, size);
        stack_maps[found] = stack_maps[--nstack_maps];
        for (uint64_t page = lo; page < hi; page += PAGE_16K) {
            int used = 0;
            for (unsigned i = 0; i < nstack_maps; i++) if (overlaps(page, PAGE_16K, stack_maps[i].dst, stack_maps[i].size)) used = 1;
            if (!used && mprotect((void *)page, PAGE_16K, PROT_NONE)) runtime_fail("decommit moved stack");
        }
    } else {
        if (nstack_maps == 4096) { pthread_mutex_unlock(&mem_lock); return R_OUT_OF_RESOURCE; }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, size, stack_maps[i].dst, stack_maps[i].size) || overlaps(src, size, stack_maps[i].src, stack_maps[i].size)) { pthread_mutex_unlock(&mem_lock); return 0xdc01; }
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) runtime_fail("commit moved stack");
        memcpy((void *)dst, (const void *)src, size);
        if (shi > slo && mprotect((void *)slo, shi - slo, PROT_NONE)) runtime_fail("protect moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, size};
    }
    pthread_mutex_unlock(&mem_lock);
    return 0;
}

/* ---- MapPhysicalMemory / UnmapPhysicalMemory (alias region) ------------------------------------------------------
 * Only the pages of the range that are still unmapped are mapped (zero-filled); mapped pages keep their contents. */
static uint64_t alias_map(int unmap, uint64_t addr, uint64_t size) {
    if (!size || ((addr | size) & 4095)) return R_INVALID_ADDRESS;
    uint64_t start = (uintptr_t)alias_space;
    if (size > alias_len || addr < start || addr - start > alias_len - size) return 0xdc01;
    pthread_mutex_lock(&mem_lock);
    unsigned first = (unsigned)((addr - start) / PAGE_4K), count = (unsigned)(size / PAGE_4K);
    uint64_t fresh = 0;
    if (!unmap) for (unsigned i = 0; i < count; i++) if (!alias_state[first + i]) fresh += PAGE_4K;
    /* Resource exhaustion is LimitReached; the SDK maps it to its recoverable out-of-memory result. */
    if (!unmap && fresh + alias_bytes + heap_size + module_image_bytes() > g_application_pool_bytes) { pthread_mutex_unlock(&mem_lock); return R_LIMIT_REACHED; }
    uint64_t lo = addr & ~(PAGE_16K - 1), hi = (addr + size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++) if (overlaps(addr, size, stack_maps[i].src, stack_maps[i].size)) { pthread_mutex_unlock(&mem_lock); return 0xdc01; }
        uint64_t released = 0;
        for (unsigned i = 0; i < count; i++) if (alias_state[first + i] == 5) { alias_state[first + i] = 0; alias_stale[first + i] = 1; released += PAGE_4K; }
        alias_bytes -= released;
        for (uint64_t page = lo; page < hi; page += PAGE_16K) {
            unsigned p = (unsigned)((page - start) / PAGE_4K);
            if (!alias_state[p] && !alias_state[p + 1] && !alias_state[p + 2] && !alias_state[p + 3]) {
                if (mmap((void *)page, PAGE_16K, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) != (void *)page) runtime_fail("decommit physical alias page");
                memset(alias_stale + p, 0, 4);
            }
        }
    } else if (fresh) {
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) runtime_fail("commit physical alias pages");
        for (unsigned i = 0; i < count; i++) if (!alias_state[first + i]) {
            if (alias_stale[first + i]) { memset((void *)(addr + (uint64_t)i * PAGE_4K), 0, PAGE_4K); alias_stale[first + i] = 0; }
            alias_state[first + i] = 5;
        }
        alias_bytes += fresh;
    }
    pthread_mutex_unlock(&mem_lock);
    return 0;
}

/* ---- code memory aliases (kcode.c) --------------------------------------------------------------------------------------
 * The guest picks the alias address in a range QueryMemory reports Free: the unused parts of the reservation around the stack region and the
 * heap cap. The alias shares the source pages (mach_vm_remap without copy), which needs 16 KiB host pages on both sides. */
int memory_is_heap(uint64_t addr, uint64_t size) {
    memory_init();
    return in_heap_region(addr, size) && size <= heap_size && addr - (uintptr_t)heap <= heap_size - size;
}
int memory_range_unused(uint64_t a, uint64_t n) {
    memory_init();
    uint64_t lo = (uintptr_t)low_space, gaps[3][2] = {{lo, lo + STACK_OFF}, {lo + STACK_OFF + STACK_SIZE, (uintptr_t)heap}, {(uintptr_t)heap + HEAP_CAP, lo + LOW_SIZE}};
    for (unsigned i = 0; i < 3; i++) if (a >= gaps[i][0] && n <= gaps[i][1] - gaps[i][0] && a - gaps[i][0] <= gaps[i][1] - gaps[i][0] - n) return 1;
    return 0;
}
uint32_t memory_code_alias(uint64_t dst, uint64_t src, uint64_t size, unsigned perm) {
    if ((dst | src | size) & (PAGE_16K - 1)) {
        LOG("memory", "code memory alias %#llx <- %#llx+%#llx: not 16 KiB aligned, refused", (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size);
        summary_note("code memory alias not host-page aligned");
        return R_INVALID_ADDRESS;
    }
    mach_vm_address_t a = dst; vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (mach_vm_address_t)src, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != dst) { LOG("memory", "code memory alias %#llx: remap failed (%d)", (unsigned long long)dst, kr); return R_OUT_OF_RESOURCE; }
    int prot = PROT_READ | (perm & 2 ? PROT_WRITE : 0) | (perm & 4 ? PROT_EXEC : 0);
    if (mprotect((void *)dst, size, prot)) {
        LOG("memory", "code memory alias %#llx: permission %u refused by the host", (unsigned long long)dst, perm);
        mmap((void *)dst, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        return R_INVALID_MEMORY_PERMISSIONS;
    }
    if (perm & 4) sys_icache_invalidate((void *)dst, size);
    return 0;
}
void memory_code_unalias(uint64_t dst, uint64_t size) {
    if (mmap((void *)dst, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) != (void *)dst) runtime_fail("code memory unmap");
}

/* ---- svc handlers --------------------------------------------------------------------------------------------- */
static void svc_set_heap_size(uint64_t *r) {
    memory_init();
    size_t size = r[1];
    if (size > HEAP_CAP || (size % 0x200000) || size + module_image_bytes() > g_application_pool_bytes) { r[0] = R_INVALID_CURRENT_MEMORY; return; }
    pthread_mutex_lock(&mem_lock);
    if (size > heap_size && mprotect(heap + heap_size, size - heap_size, PROT_READ | PROT_WRITE)) runtime_fail("commit heap: %zu bytes", size);
    if (size < heap_size && mprotect(heap + size, heap_size - size, PROT_NONE)) runtime_fail("decommit heap");
    heap_size = size;
    pthread_mutex_unlock(&mem_lock);
    LOGV("memory", "SetHeapSize %#zx", size);
    r[0] = 0; r[1] = (uintptr_t)heap;
}
static void svc_set_memory_permission(uint64_t *r) {
    uint64_t res = module_set_permission(r[0], r[1], (unsigned)r[2]);
    if (res == R_INVALID_CURRENT_MEMORY && (in_heap_region(r[0], r[1]) || in_alias(r[0], r[1]))) {
        LOGV("memory", "SetMemoryPermission %#llx+%#llx perm %llu on heap/alias memory ignored", (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2]);
        res = 0;
    }
    r[0] = res;
}
static void svc_set_memory_attribute(uint64_t *r) {
    if (r[2] == 8) {   /* Uncached (the SDK marks GPU-visible memory so): a no-op here, host memory is coherent for the GPU through unified memory */
        if (r[0] & 4095) r[0] = R_INVALID_ADDRESS; else if (!r[1] || (r[1] & 4095)) r[0] = R_INVALID_SIZE;
        else r[0] = memory_contains(r[0], r[1]) ? 0 : R_INVALID_CURRENT_MEMORY;
        return;
    }
    uint64_t res = module_set_attribute(r[0], r[1], (unsigned)r[2], (unsigned)r[3]);
    if (res == R_INVALID_CURRENT_MEMORY && (in_heap_region(r[0], r[1]) || in_alias(r[0], r[1]))) res = 0;   /* uncached / locked attributes: no host equivalent */
    r[0] = res;
}
static void svc_map_memory(uint64_t *r) { r[0] = stack_move(0, r[0], r[1], r[2]); }
static void svc_unmap_memory(uint64_t *r) { r[0] = stack_move(1, r[0], r[1], r[2]); }
static void svc_map_physical_memory(uint64_t *r) {
    memory_init();
    uint64_t a = r[0], n = r[1]; r[0] = alias_map(0, a, n);
    if (r[0]) LOG("memory", "MapPhysicalMemory %#llx+%#llx failed %#llx (alias used %#llx, heap %#llx)", (unsigned long long)a, (unsigned long long)n, (unsigned long long)r[0], (unsigned long long)alias_bytes, (unsigned long long)heap_size);
}
static void svc_unmap_physical_memory(uint64_t *r) { memory_init(); r[0] = alias_map(1, r[0], r[1]); }

/* QueryMemory(x0 MemoryInfo*, x1 PageInfo*, x2 address) -> x0 result, x1 page info. States: 0 Free, 3 Code, 4 CodeData, 5 Normal,
 * 6 Shared, 0xb Stack, 0xd Transfered, 0x10 Inaccessible. */
static void svc_query_memory(uint64_t *r) {
    memory_init();
    uint64_t addr = r[2], base = addr & ~0xfffULL, size = 0x1000; unsigned state = 0x10, perm = 0, attr = 0;
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self()); size_t stack = pthread_get_stacksize_np(pthread_self());
    uint64_t lowest = (uintptr_t)low_space;
    if (addr >= top - stack && addr < top) { base = top - stack; size = stack; state = 0xb; perm = 3; }
    else if (addr >= (uintptr_t)heap && addr < (uintptr_t)heap + HEAP_CAP) {
        if (addr < (uintptr_t)heap + heap_size) { base = (uintptr_t)heap; size = heap_size; state = 5; perm = 3; }
        else { base = (uintptr_t)heap + heap_size; size = HEAP_CAP - heap_size; state = 0; perm = 0; }
    } else if (addr >= lowest && addr < lowest + LOW_SIZE) {
        /* gaps between stack/heap in the reserved range: free for the guest address space manager */
        uint64_t b = lowest, e = lowest + LOW_SIZE;
        if (addr < (uintptr_t)heap) { e = (uintptr_t)heap; } else { b = (uintptr_t)heap + HEAP_CAP; }
        base = b; size = e - b; state = 0; perm = 0;
    } else if (!module_query(addr, &base, &size, &state, &perm, &attr)) {
        if (addr < lowest) { base = 0; size = lowest; state = 0; perm = 0; }
        else if (!(addr >= (uintptr_t)alias_space && addr < (uintptr_t)alias_space + alias_len)) { size = 0x1000; state = 0x10; perm = 0; }
    }
    pthread_mutex_lock(&mem_lock);
    if (addr >= (uintptr_t)alias_space && addr < (uintptr_t)alias_space + alias_len) {
        unsigned p = (unsigned)((addr - (uintptr_t)alias_space) / PAGE_4K), lo = p, hi = p + 1, st = alias_state[p];
        while (lo > 0 && alias_state[lo - 1] == st) lo--;
        while (hi < sizeof alias_state && hi < alias_len / PAGE_4K && alias_state[hi] == st) hi++;
        base = (uintptr_t)alias_space + (uint64_t)lo * PAGE_4K; size = (uint64_t)(hi - lo) * PAGE_4K; state = st; perm = st ? 3 : 0;
    }
    for (unsigned i = 0; i < nstack_maps; i++) {
        StackMap *m = &stack_maps[i]; uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (addr >= m->dst && addr < end) { base = m->dst; size = m->size; state = 0xb; perm = 3; break; }
        if (addr >= m->src && addr < send) { base = m->src; size = m->size; state = 5; perm = 0; attr = 4; break; }
        if (state == 0) { uint64_t old_end = base + size; if (end <= addr && end > base) base = end; if (m->dst > addr && m->dst < old_end) old_end = m->dst; size = old_end - base; }
        else if (state == 5) { uint64_t old_end = base + size; if (send <= addr && send > base) base = send; if (m->src > addr && m->src < old_end) old_end = m->src; size = old_end - base; }
    }
    pthread_mutex_unlock(&mem_lock);
    { uint64_t cb, cs; unsigned cst, cp; if (kcode_query(addr, &cb, &cs, &cst, &cp)) { base = cb; size = cs; state = cst; perm = cp; attr = 0; } }
    struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } info = {base, size, state, attr, perm, 0, 0, 0};
    void *out = guest_ptr(r[0], sizeof info);
    if (!out) { r[0] = R_INVALID_POINTER; return; }
    memcpy(out, &info, sizeof info);
    r[0] = 0; r[1] = 0;
}

void svc_memory_register(void) {
    svc_register(0x01, "SetHeapSize", svc_set_heap_size);
    svc_register(0x02, "SetMemoryPermission", svc_set_memory_permission);
    svc_register(0x03, "SetMemoryAttribute", svc_set_memory_attribute);
    svc_register(0x04, "MapMemory", svc_map_memory);
    svc_register(0x05, "UnmapMemory", svc_unmap_memory);
    svc_register(0x06, "QueryMemory", svc_query_memory);
    svc_register(0x2C, "MapPhysicalMemory", svc_map_physical_memory);
    svc_register(0x2D, "UnmapPhysicalMemory", svc_unmap_physical_memory);
    svc_kcode_register();
}
