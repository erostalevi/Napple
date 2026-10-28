/* Fuzz target: NRO / NRR validation (src/nro.c) as the ro service applies it to the images and registration lists a guest hands over. The sizes the guest
 * passes next to the buffers are modelled by also running each input with the size fields of its header patched to the real length. */
#include "../../src/nro.h"
#include "../../src/sha256.h"
#include "fuzz.h"
#include <stdlib.h>
#include <string.h>

int fuzz_nro(const uint8_t *d, size_t n) {
    NroInfo ni;
    NrrInfo ri;
    uint8_t hash[32];
    FzIn r = {d, n, 0};
    uint64_t pid = fz_u32(&r) | (uint64_t)fz_u32(&r) << 32;
    int kind = (int)(fz_u8(&r) % 3) - 1;
    nro_validate(d, n, 0, &ni);
    nro_validate(d, n, n >= 0x3c ? ((uint32_t)d[0x38] | (uint32_t)d[0x39] << 8 | (uint32_t)d[0x3a] << 16 | (uint32_t)d[0x3b] << 24) : 0, &ni);
    nrr_validate(d, n, 0, -1, &ri);
    nrr_validate(d, n, n, kind, &ri);
    sha256(d, n, hash);
    nro_id_hex(hash, n % 40, (char[65]){0});
    if (n >= 0x350) { /* the guest's size fields agree with the buffer: reaches the checks behind them */
        uint8_t *c = malloc(n);
        memcpy(c, d, n);
        uint32_t sz = (uint32_t)n;
        memcpy(c + 0x18, &sz, 4);
        uint32_t bss;
        memcpy(&bss, c + 0x38, 4);
        if (nro_validate(c, n, bss, &ni) == 0) nro_validate(c, n, bss + 0x1000, &ni);
        memcpy(c + 0x338, &sz, 4);
        if (nrr_validate(c, n, 0, -1, &ri) == 0 && ri.nhashes) {
            nrr_has_hash(c, &ri, hash);
            nrr_has_hash(c, &ri, c + ri.hashes_off);
            nrr_has_hash(c, &ri, c + ri.hashes_off + (ri.nhashes - 1) * 32);
        }
        uint64_t mask, pattern;
        memcpy(&mask, c + 0x10, 8), memcpy(&pattern, c + 0x18, 8);
        nrr_validate(c, n, ri.program_id, kind, &ri);
        free(c);
    }
    return 0;
}

void fuzz_nro_seeds(FuzzSeedFn add, void *ctx) {
    uint8_t nro[0xC000];
    memset(nro, 0, sizeof nro);
    memcpy(nro + 0x10, "NRO0", 4);
    uint32_t v[] = {sizeof nro, 0, 0, 0x4000, 0x4000, 0x4000, 0x8000, 0x4000, 0x1000};
    memcpy(nro + 0x18, &v[0], 4);
    memcpy(nro + 0x20, &v[2], 4 * 7);
    for (unsigned i = 0x40; i < 0x60; i++) nro[i] = (uint8_t)i;
    add(ctx, nro, 0x100); /* the header is what the validator reads */
    add(ctx, nro, 0x80);
    uint8_t nrr[0x1000];
    memset(nrr, 0, sizeof nrr);
    memcpy(nrr, "NRR0", 4);
    uint64_t mask = ~0ULL, pid = 0x0100000000010000ULL;
    memcpy(nrr + 0x10, &mask, 8), memcpy(nrr + 0x18, &pid, 8), memcpy(nrr + 0x330, &pid, 8);
    uint32_t size = sizeof nrr, off = 0x350, cnt = 4;
    memcpy(nrr + 0x338, &size, 4), memcpy(nrr + 0x340, &off, 4), memcpy(nrr + 0x344, &cnt, 4);
    for (unsigned i = 0; i < 4; i++) memset(nrr + off + 32 * i, 0x10 * (i + 1), 32);
    add(ctx, nrr, 0x3d0);
    add(ctx, nrr, sizeof nrr);
}
FUZZ_TARGET(nro);
