/* t_vpool.c - virtual memory pools: a physical pool (storage only, 4 KiB aligned but not 16 KiB aligned like the real guest heap) is mapped into a virtual
 * pool with nvnMemoryPoolMapVirtual; a GPU clear through the virtual address must show up in the physical storage (page aliasing), an unmap must drop
 * the alias, and a remap to another offset must expose the same physical bytes at the new place. */
#include "nvn_test.h"

typedef struct { void *phys; int64_t poff, voff, size; int32_t storage_class, pad; } mapreq_t;   /* NVNmappingRequest layout (see api_memory.c) */

static void gpu_clear(NVNdevice *dev, NVNqueue *q, uint64_t addr, size_t n, uint32_t v) {
    NVNcommandBuffer *cb = calloc(1, sizeof(NVNcommandBuffer));
    L(nvnCommandBufferInitialize)(cb, dev);
    L(nvnCommandBufferBeginRecording)(cb);
    L(nvnCommandBufferClearBuffer)(cb, (NVNbufferAddress)addr, n, v);
    NVNcommandHandle h = L(nvnCommandBufferEndRecording)(cb);
    L(nvnQueueSubmitCommands)(q, 1, &h);
    L(nvnQueueFinish)(q);
    L(nvnCommandBufferFinalize)(cb);
}

int main(void) {
    napple_gfx_config cfg; napple_gfx_config_default(&cfg);
    cfg.headless_width = 16; cfg.headless_height = 16;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    NVNdevice *dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db); L(nvnDeviceInitialize)(dev, &db);

    const size_t PSZ = 0x100000;
    uint8_t *raw = mmap(NULL, PSZ + 0x8000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint8_t *phys_mem = raw + 0x4000 + 0x1000;                     /* 4 KiB aligned only */
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, phys_mem, PSZ);
    L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS);
    NVNmemoryPool *phys = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(phys, &pb), "physical pool");

    const size_t VSZ = 0x400000;
    L(nvnMemoryPoolBuilderSetStorage)(&pb, NULL, VSZ);
    L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    NVNmemoryPool *virt = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(virt, &pb), "virtual pool");
    uint64_t vbase = (uint64_t)L(nvnMemoryPoolGetBufferAddress)(virt);
    CHECK(vbase != 0, "virtual pool has a GPU address");

    NVNqueueBuilder qb; L(nvnQueueBuilderSetDevice)(&qb, dev); L(nvnQueueBuilderSetDefaults)(&qb);
    NVNqueue *q = calloc(1, sizeof(NVNqueue)); L(nvnQueueInitialize)(q, &qb);

    /* map physical [0x10000, 0x30000) at virtual 0x20000 */
    mapreq_t r = { phys, 0x10000, 0x20000, 0x20000, 0, 0 };
    CHECK(L(nvnMemoryPoolMapVirtual)(virt, 1, (const NVNmappingRequest *)&r), "MapVirtual");
    gpu_clear(dev, q, vbase + 0x20000, 0x1000, 0xabcd1234u);
    CHECK(((volatile uint32_t *)(phys_mem + 0x10000))[0] == 0xabcd1234u, "GPU write through the virtual address reaches physical storage (got %08x)", ((volatile uint32_t *)(phys_mem + 0x10000))[0]);
    CHECK(((volatile uint32_t *)(phys_mem + 0x10000 + 0x2000))[0] == 0, "bytes outside the cleared range are untouched");

    /* the same physical range again, at another virtual place */
    mapreq_t r2 = { phys, 0x10000, 0x100000, 0x20000, 0, 0 };
    CHECK(L(nvnMemoryPoolMapVirtual)(virt, 1, (const NVNmappingRequest *)&r2), "second MapVirtual");
    gpu_clear(dev, q, vbase + 0x100000 + 0x4000, 0x1000, 0x5a5a5a5au);
    CHECK(((volatile uint32_t *)(phys_mem + 0x10000 + 0x4000))[0] == 0x5a5a5a5au, "second alias writes the same physical bytes");
    CHECK(((volatile uint32_t *)(phys_mem + 0x10000))[0] == 0xabcd1234u, "earlier data is preserved by the remap");

    /* unmap the first alias: later GPU writes there must not reach the storage */
    mapreq_t un = { NULL, 0, 0x20000, 0x20000, 0, 0 };
    CHECK(L(nvnMemoryPoolMapVirtual)(virt, 1, (const NVNmappingRequest *)&un), "unmap request");
    uint32_t keep = ((volatile uint32_t *)(phys_mem + 0x10000 + 0x8000))[0];
    gpu_clear(dev, q, vbase + 0x100000 + 0x8000, 0x1000, 0x11111111u);      /* still mapped */
    CHECK(((volatile uint32_t *)(phys_mem + 0x10000 + 0x8000))[0] == 0x11111111u && keep == 0, "remaining alias still works after the unmap");

    L(nvnMemoryPoolFinalize)(virt);
    L(nvnMemoryPoolFinalize)(phys);
    napple_gfx_stats st; napple_gfx_get_stats(&st);
    CHECK(st.unimplemented_calls_total == 0, "unimplemented NVN calls: %llu", (unsigned long long)st.unimplemented_calls_total);
    printf(g_fail ? "t_vpool: %d FAILED\n" : "t_vpool: ok\n", g_fail);
    return g_fail != 0;
}
