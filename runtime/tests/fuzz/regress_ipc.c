/* Regression tests for the crashes the ipc fuzz target found (FINDINGS.md). Linux only (links the services with the host stand-ins).
 * Build with the sanitizers: make -C runtime fuzz-regress. Each case is one record of the target's input: [service][flags][len u16][message]. */
#include "t_ipc.c"
#include "../check.h"
#include "../../src/services/time_core.h"

extern char fuzz_fail_msg[256];
static void run_record(unsigned svc, unsigned flags, const uint32_t *words, size_t nwords) {
    uint8_t in[4 + 0x100 + 64];
    memset(in, 0, sizeof in);
    in[0] = (uint8_t)svc;
    in[1] = (uint8_t)flags;
    fz_put16(in + 2, (uint16_t)(nwords * 4));
    memcpy(in + 4, words, nwords * 4);
    fuzz_ipc(in, 4 + nwords * 4);
}

int main(void) {
    uint32_t m[64];

    /* 1. The data size and receive list of a request are the guest's: ndata 0x3ff words with a receive buffer put the receive list 4 KiB after the 256 byte
     * message buffer, and parse() read it (ASan: heap-buffer-overflow, ipc.c parse) before checking the size. Found by fuzz ipc, seed 1, iteration 8. */
    memset(m, 0, sizeof m);
    m[0] = 4;
    m[1] = 0x3ff | (2u << 10);
    fuzz_fail_msg[0] = 0;
    run_record(0, 0, m, 64);
    CHECK(strstr(fuzz_fail_msg, "message buffer") != NULL, "the oversized request is refused (runtime_fail: '%s')", fuzz_fail_msg);

    /* 2. Descriptor counts beyond what the buffer holds: 8 each of X, A, B and W descriptors, a special header with a pid and 8 + 8 handles. */
    memset(m, 0, sizeof m);
    m[0] = 4 | 8u << 16 | 8u << 20 | 8u << 24 | 8u << 28;
    m[1] = 1u << 31;
    m[2] = 1 | 8u << 1 | 8u << 5;
    run_record(0, 0, m, 64);
    fuzz_fail_msg[0] = 0;
    run_record(0, 0, m, 64);
    CHECK(strstr(fuzz_fail_msg, "message buffer") != NULL, "too many descriptors are refused (runtime_fail: '%s')", fuzz_fail_msg);
    m[1] = 8 | 1u << 31; /* with data and a receive list too */
    run_record(0, 0, m, 64);

    /* 3. A domain request whose descriptors leave no room for the domain and CMIF headers: payload + 32 is past the buffer. */
    memset(m, 0, sizeof m);
    m[0] = 4 | 4u << 16 | 8u << 20 | 8u << 24;
    fuzz_fail_msg[0] = 0;
    run_record(0, 1, m, 64);
    CHECK(strstr(fuzz_fail_msg, "message buffer") != NULL, "a domain request without room for its headers is refused (runtime_fail: '%s')", fuzz_fail_msg);
    m[0] = 4 | 3u << 16 | 8u << 20 | 8u << 24; /* payload at 0xe0: fits a plain header, not a domain one */
    run_record(0, 0, m, 64);
    run_record(0, 1, m, 64);

    /* 4. A domain message whose data size puts the input object ids far past the buffer (ids = payload + 16 + data size). */
    memset(m, 0, sizeof m);
    m[0] = 4;
    m[1] = 12;
    uint8_t *payload = (uint8_t *)m + 16;
    payload[0] = 1;                     /* send */
    payload[1] = 8;                     /* input objects */
    fz_put16(payload + 2, 0xffff);      /* data size */
    fz_put32(payload + 4, 1);           /* the converted root object */
    fz_put32(payload + 16, 0x49434653); /* SFCI */
    run_record(0, 1, m, 64);

    /* 5. A TIPC request puts its arguments at a 4 byte boundary and IPC_IN read them through a typed pointer (UBSan: load of misaligned address for type
     * 'const uint64_t', pl.c:227). Found by fuzz ipc, seed 101, iteration 50095 (12 byte input). IPC_IN copies now. */
    static const uint8_t tipc_misaligned[] = {0x1d, 0x00, 0x50, 0x00, 0x16, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0xff};
    fuzz_ipc(tipc_misaligned, sizeof tipc_misaligned);

    /* 6. bsd Select: the timeout is sec * 1000 + usec / 1000 on guest int64 values (UBSan: signed integer overflow, offline.c:774). Found by fuzz ipc, seed 103,
     * iteration 164692 (180 byte input). */
    static const uint8_t select_timeout[] = {
        0x27, 0x00, 0x78, 0x00, 0x04, 0x00, 0x11, 0x01, 0x12, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x46, 0x43, 0x49, 0x00, 0x04, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
        0x00, 0x00, 0x00, 0x53, 0x46, 0x43, 0x49, 0x00, 0x00, 0x00, 0x00, 0x05, 0xc9, 0xd0, 0xd7, 0xde, 0xe5, 0xec, 0xf3, 0xfa, 0x01, 0x08, 0x0f,
        0x16, 0x1d, 0x24, 0x2b, 0x32, 0x39, 0x40, 0x47, 0x4e, 0x55, 0x5c, 0x63, 0x6a, 0x71, 0x78, 0x7f, 0x86, 0x8d, 0x94, 0x9b, 0xa3, 0xaa, 0xb1,
        0xb8, 0xbf, 0xc6, 0xcd, 0xd4, 0xdb, 0xe2, 0xe9, 0xf0, 0xf7, 0xfe, 0x05, 0x0c, 0x13, 0x1a, 0x21, 0x28, 0x2f, 0x36, 0x3d, 0x44, 0x4b, 0x52,
        0x59, 0x60, 0x67, 0xcf, 0xfa, 0x41, 0xe3, 0x1d, 0x07, 0xab, 0x9b, 0x5d, 0x9f, 0x82, 0xf9, 0x3b, 0x55, 0x25, 0xf2, 0x69, 0xbe, 0x8e, 0x87,
        0xb8, 0x97, 0x28, 0x65, 0x74, 0xfc, 0xfb, 0xef, 0x24, 0x16, 0x18, 0x45, 0x30, 0x89, 0x4d, 0x65, 0xa0, 0xbe, 0xbc};
    fuzz_ipc(select_timeout, sizeof select_timeout);

    /* 7. Guest-supplied time snapshots: the span between two user times and the nanosecond conversion overflowed int64 (time_core.c:153, time.c:353/374). */
    TimeSnapshot a, b;
    int64_t span = 0;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.user_time = INT64_MIN + 5;
    b.user_time = INT64_MAX - 5;
    CHECK(time_snapshot_span(&a, &b, &span) == 0, "span of extreme snapshots is computed (wrapped)");

    printf("{\"test\":\"regress_ipc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
