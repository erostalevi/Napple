/* Host unit test: svc 0x23 SendAsyncRequestWithUserBuffer delivers like 0x22 and returns an already signalled readable event. */
#include "napple.h"
#include "host_stubs.h"
#include "../check.h"

void svc_kernel_register(void);
static uint64_t svc(unsigned num, uint64_t *r) { svc_dispatch(num, r, "test", 0x40); return r[0]; }

static uint64_t seen[3]; static uint64_t sync_result;
static void fake_sync(uint64_t *r) { seen[0] = r[0]; seen[1] = r[1]; seen[2] = r[2]; r[0] = sync_result; }
static uint8_t tlr[0x200];

int main(void) {
    svc_kernel_register(); svc_threads_register();
    host_thread_attach(tlr);
    uint64_t r[9] = {0};
    r[1] = 0x1000; r[2] = 0x200; r[3] = 0x55; r[0] = 0;
    CHECK(svc(0x23, r) == R_NOT_IMPLEMENTED, "no synchronous path registered: clean error");

    svc_register(0x22, "SendSyncRequestWithUserBuffer", fake_sync);
    memset(r, 0, sizeof r); r[1] = 0x1000; r[2] = 0x200; r[3] = 0x55;
    CHECK(svc(0x23, r) == 0 && r[1], "success returns an event handle");
    CHECK(seen[0] == 0x1000 && seen[1] == 0x200 && seen[2] == 0x55, "arguments are remapped to the 0x22 layout (addr, size, handle)");
    uint32_t ev = (uint32_t)r[1];
    CHECK(event_is_signaled(ev), "the readable event is already signalled");
    uint64_t c[9] = {ev}; CHECK(svc(0x17, c) == 0 && !event_is_signaled(ev), "it is a normal readable event (ResetSignal clears it)");
    c[0] = ev; CHECK(svc(0x16, c) == 0, "and can be closed");

    sync_result = R_INVALID_HANDLE;
    memset(r, 0, sizeof r); r[1] = 0x1000; r[2] = 0x200; r[3] = 0x55;
    CHECK(svc(0x23, r) == R_INVALID_HANDLE && r[1] == 0x1000, "a failing request returns its error and no event");
    printf("{\"test\":\"svc_async\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
