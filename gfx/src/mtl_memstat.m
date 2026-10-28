/* mtl_memstat.m - how much memory the Metal device has handed out (textures, buffers, pipelines): the GPU half of the runtime's memory log. */
#import "mtl_priv.h"

uint64_t gfx_mtl_allocated_bytes(void) {
    id<MTLDevice> d = g_mtl.dev;
    return d ? (uint64_t)[d currentAllocatedSize] : 0;
}
