/* t_sparse.c - sparse textures (NVN_TEXTURE_FLAGS_SPARSE on a virtual memory pool), the documented fallback of api_texture.c: the Metal texture is fully resident
 * and reads the guest storage through the virtual pool, so a tile mapped with nvnMemoryPoolMapVirtual shows its physical pages and an unmapped tile reads as
 * zero (the reservation is zero-filled), as on the console. A 256x256 RGBA8 sparse texture has 64 KiB tiles of 128x128 texels; tile (0, 0) is the first
 * 64 KiB of its block-linear storage. Tile (1, 0) is mapped afterwards and drawn into by the GPU (a copy): the texture keeps it.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"
#include <sys/mman.h>

typedef struct { void *phys; int64_t poff, voff, size; int32_t storage_class, pad; } mapreq_t; /* NVNmappingRequest (api_memory.c) */
enum { OFF_SRC = 0x100000, OFF_RB = H_READBACK };

static H h;
static uint32_t px(int x, int y) { return ((const uint32_t *)(h.mem + OFF_RB))[y * 256 + x]; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, 16, 16, NULL)) return 1;
    const size_t PSZ = 0x40000, VSZ = 0x100000;
    uint8_t *phys_mem = mmap(NULL, PSZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    for (size_t i = 0; i < 0x10000 / 4; i++) ((uint32_t *)phys_mem)[i] = 0xff336699u;
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, h.dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, phys_mem, PSZ);
    L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS);
    NVNmemoryPool *phys = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(phys, &pb), "physical pool");
    L(nvnMemoryPoolBuilderSetStorage)(&pb, NULL, VSZ);
    L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    NVNmemoryPool *virt = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(virt, &pb), "virtual pool");

    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetSize2D)(&tb, 256, 256); L(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_SPARSE);
    struct { int32_t levels, w, hgt, d, reserved[4]; } lay;
    L(nvnTextureBuilderGetSparseTileLayout)(&tb, (NVNtextureSparseTileLayout *)&lay);
    CHECK(lay.w == 128 && lay.hgt == 128, "64 KiB tiles of 128x128 RGBA8 texels: %dx%d", lay.w, lay.hgt);
    L(nvnTextureBuilderSetStorage)(&tb, virt, 0);
    NVNtexture *t = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t, &tb), "sparse texture on the virtual pool");

    /* tile (0, 0) <- physical [0, 64 KiB) */
    mapreq_t r = {phys, 0, 0, 0x10000, 0, 0};
    CHECK(L(nvnMemoryPoolMapVirtual)(virt, 1, (const NVNmappingRequest *)&r), "map tile (0, 0)");
    h_begin(&h);
    memset(h.mem + OFF_RB, 0xcd, 256 * 256 * 4);
    h_readback(&h, t, 0, 0, 256, 256, OFF_RB);
    h_submit(&h);
    printf("tile (0,0) %08x %08x | tile (1,0) %08x | tile (0,1) %08x | tile (1,1) %08x\n", px(0, 0), px(127, 127), px(128, 0), px(0, 128), px(200, 200));
    CHECK(px(0, 0) == 0xff336699u && px(127, 127) == 0xff336699u && px(64, 5) == 0xff336699u, "mapped tile (0, 0) reads its physical pages: %08x %08x", px(0, 0), px(127, 127));
    CHECK(px(128, 0) == 0 && px(0, 128) == 0 && px(255, 255) == 0, "unmapped tiles read as zero: %08x %08x %08x", px(128, 0), px(0, 128), px(255, 255));

    /* tile (1, 0) <- physical [64 KiB, 128 KiB), filled by a GPU copy of a 128x128 green block */
    mapreq_t r2 = {phys, 0x10000, 0x10000, 0x10000, 0, 0};
    CHECK(L(nvnMemoryPoolMapVirtual)(virt, 1, (const NVNmappingRequest *)&r2), "map tile (1, 0)");
    for (int i = 0; i < 128 * 128; i++) ((uint32_t *)(h.mem + OFF_SRC))[i] = 0xff00ff00u;
    NVNcopyRegion cr = {128, 0, 0, 128, 128, 1};
    h_begin(&h);
    L(nvnCommandBufferCopyBufferToTexture)(h.cb, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_SRC), t, NULL, &cr, 0);
    memset(h.mem + OFF_RB, 0xcd, 256 * 256 * 4);
    h_readback(&h, t, 0, 0, 256, 256, OFF_RB);
    h_submit(&h);
    CHECK(px(130, 3) == 0xff00ff00u && px(255, 127) == 0xff00ff00u, "GPU copy into the newly mapped tile (1, 0): %08x %08x", px(130, 3), px(255, 127));
    CHECK(px(5, 5) == 0xff336699u && px(5, 200) == 0, "the other tiles are unchanged: %08x %08x", px(5, 5), px(5, 200));
    return h_finish("t_sparse", &h);
}
