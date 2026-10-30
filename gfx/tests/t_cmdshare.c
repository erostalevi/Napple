/* t_cmdshare.c - a guest command buffer object misused the way ACNH's start did (src/api_cmd.c): the object lives on a stack, is used by two threads at once,
 * is finalized and initialised again under the other thread, is byte-copied, and is overwritten with stack garbage. None of this may touch freed or foreign
 * host memory: calls on a dead or foreign object are ignored, large commands that are not recorded go to a sink as large as the command, and the lists that
 * do get recorded stay well formed and callable. No game data. Run it with MallocScribble=1 MallocPreScribble=1 to make a stray free or overflow fault. */
#include <pthread.h>
#include <stdatomic.h>
#include "nvn_harness.h"

int gfx_cmdrec_valid(uint64_t handle);

static NVNdevice *g_dev;
static NVNcommandBuffer *g_shared;           /* the object both threads use (like ACNH's context->cb on the main thread's stack) */
static _Atomic int g_stop;
static _Atomic uint64_t g_lists;
static NVNcommandHandle g_callee;
static uint8_t g_ubo[16384];

/* One thread's loop: re-create the object, record a few commands (some large), end and drop the list. The other thread runs the same loop on the same
 * object, so every step may meet the other's Initialize, Finalize, BeginRecording or EndRecording. */
static void *hammer(void *arg) {
    int finalize = (int)(intptr_t)arg;
    NVNcommandHandle callees[64];
    for (int i = 0; i < 64; i++) callees[i] = g_callee;
    for (unsigned i = 0; !atomic_load(&g_stop); i++) {
        if (finalize) {
            L(nvnCommandBufferFinalize)(g_shared);
            L(nvnCommandBufferInitialize)(g_shared, g_dev);
        }
        L(nvnCommandBufferBeginRecording)(g_shared);
        L(nvnCommandBufferUpdateUniformBuffer)(g_shared, 0x1000, sizeof g_ubo, 0, 64 + (i % 64) * 256, g_ubo);
        L(nvnCommandBufferCallCommands)(g_shared, 1 + (int)(i % 64), callees);
        L(nvnCommandBufferSetDepthBounds)(g_shared, 1, 0.25f, 0.75f);
        NVNcommandHandle hd = L(nvnCommandBufferEndRecording)(g_shared);
        if (hd) { L(nvnDeviceFinalizeCommandHandle)(g_dev, hd); atomic_fetch_add(&g_lists, 1); }
    }
    return NULL;
}

int main(void) {
    H h; if (h_init(&h, 16, 16, NULL)) return 1;
    g_dev = h.dev;
    for (size_t i = 0; i < sizeof g_ubo; i++) g_ubo[i] = (uint8_t)i;
    h_begin(&h); L(nvnCommandBufferSetDepthBounds)(h.cb, 0, 0, 1); g_callee = L(nvnCommandBufferEndRecording)(h.cb);
    CHECK(g_callee && gfx_cmdrec_valid(g_callee), "callee list");
    NVNcommandHandle many[200];
    for (int i = 0; i < 200; i++) many[i] = g_callee;

    /* 1. never initialised (zeroed) and finalized objects: large commands go nowhere and nothing is retained */
    NVNcommandBuffer *zero = calloc(1, sizeof(NVNcommandBuffer));
    L(nvnCommandBufferUpdateUniformBuffer)(zero, 0x1000, sizeof g_ubo, 0, sizeof g_ubo, g_ubo);
    L(nvnCommandBufferCallCommands)(zero, 200, many);
    CHECK(L(nvnCommandBufferEndRecording)(zero) == 0, "EndRecording on a zeroed object");
    NVNcommandBuffer *fin = calloc(1, sizeof(NVNcommandBuffer));
    CHECK(L(nvnCommandBufferInitialize)(fin, h.dev), "init");
    L(nvnCommandBufferFinalize)(fin);
    L(nvnCommandBufferUpdateUniformBuffer)(fin, 0x1000, sizeof g_ubo, 0, sizeof g_ubo, g_ubo);
    L(nvnCommandBufferCallCommands)(fin, 200, many);

    /* 2. a live object outside Begin/End: a 16 KiB command used to land in a 256-byte field of the host part */
    NVNcommandBuffer *idle = calloc(1, sizeof(NVNcommandBuffer));
    CHECK(L(nvnCommandBufferInitialize)(idle, h.dev), "init");
    L(nvnCommandBufferUpdateUniformBuffer)(idle, 0x1000, sizeof g_ubo, 0, sizeof g_ubo, g_ubo);
    L(nvnCommandBufferCallCommands)(idle, 200, many);
    L(nvnCommandBufferBeginRecording)(idle);
    L(nvnCommandBufferSetDepthBounds)(idle, 1, 0.5f, 1);
    NVNcommandHandle ok = L(nvnCommandBufferEndRecording)(idle);
    CHECK(ok && gfx_cmdrec_valid(ok), "a list recorded after the ignored commands");

    /* 3. a byte copy and stack garbage: neither reaches the host part */
    NVNcommandBuffer *copy = malloc(sizeof(NVNcommandBuffer));
    memcpy(copy, idle, sizeof *copy);
    L(nvnCommandBufferBeginRecording)(copy);
    L(nvnCommandBufferUpdateUniformBuffer)(copy, 0x1000, sizeof g_ubo, 0, sizeof g_ubo, g_ubo);
    CHECK(L(nvnCommandBufferEndRecording)(copy) == 0, "EndRecording on a byte copy");
    L(nvnCommandBufferFinalize)(copy);                               /* must not free the original's host part */
    L(nvnCommandBufferBeginRecording)(idle);
    NVNcommandHandle ok2 = L(nvnCommandBufferEndRecording)(idle);
    CHECK(ok2 && gfx_cmdrec_valid(ok2), "the original still records after its copy was finalized");
    L(nvnCommandBufferFinalize)(idle);
    for (uint32_t pat = 0; pat < 64; pat++) {                        /* the dead object's memory reused by stack frames */
        uint32_t *w = (uint32_t *)idle;
        for (size_t i = 0; i < sizeof(NVNcommandBuffer) / 4; i++) w[i] = pat * 0x9E3779B1u + (uint32_t)i * 0x85EBCA6Bu;
        if (pat & 1) w[0] = 0x30444d43;                              /* the magic, by chance */
        if (pat & 2) w[2] = pat, w[3] = 0;                           /* a plausible slot with a stale generation */
        L(nvnCommandBufferBeginRecording)(idle);
        L(nvnCommandBufferUpdateUniformBuffer)(idle, 0x1000, sizeof g_ubo, 0, sizeof g_ubo, g_ubo);
        L(nvnCommandBufferEndRecording)(idle);
        L(nvnCommandBufferFinalize)(idle);
    }

    /* 4. two threads on one object: one re-creates it every round, both record into it */
    g_shared = calloc(1, sizeof(NVNcommandBuffer));
    CHECK(L(nvnCommandBufferInitialize)(g_shared, h.dev), "init shared");
    pthread_t a, b;
    pthread_create(&a, NULL, hammer, (void *)(intptr_t)1);
    pthread_create(&b, NULL, hammer, (void *)(intptr_t)0);
    usleep(1500000);
    atomic_store(&g_stop, 1);
    pthread_join(a, NULL); pthread_join(b, NULL);
    printf("t_cmdshare: %llu lists recorded by two threads sharing one command buffer\n", (unsigned long long)atomic_load(&g_lists));
    CHECK(atomic_load(&g_lists) > 0, "lists recorded");
    L(nvnCommandBufferFinalize)(g_shared);

    /* the device still works: a list that calls the callee replays */
    CHECK(gfx_cmdrec_valid(g_callee), "callee survives");
    h_begin(&h); L(nvnCommandBufferCallCommands)(h.cb, 1, &g_callee); NVNcommandHandle caller = L(nvnCommandBufferEndRecording)(h.cb);
    CHECK(caller != 0, "caller list");
    L(nvnQueueSubmitCommands)(h.q, 1, &caller);
    h_begin(&h); h_submit(&h);
    return h_finish("t_cmdshare", &h);
}
