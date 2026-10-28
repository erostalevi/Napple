/* Fuzz drivers for the untrusted-input C parsers (runtime/tests/fuzz). Every target file defines
 *     int fuzz_<name>(const uint8_t *data, size_t size);          one input; returns 0 (valid code never crashes, hangs or trips ASan/UBSan)
 *     void fuzz_<name>_seeds(FuzzSeedFn add, void *ctx);          the seed corpus: valid inputs built in memory, handed to `add`
 * and ends with FUZZ_TARGET(<name>). Two entry points are built per target: fuzz_libfuzzer.c (LLVMFuzzerTestOneInput, clang -fsanitize=fuzzer) and
 * fuzz_mut.c (seeded mutation driver, also replays files). See HOWTO.md. */
#ifndef RT_FUZZ_H
#define RT_FUZZ_H
#include <stddef.h>
#include <stdint.h>

typedef void (*FuzzSeedFn)(void *ctx, const uint8_t *data, size_t size);
typedef struct {
    const char *name;
    int (*run)(const uint8_t *data, size_t size);
    void (*seeds)(FuzzSeedFn add, void *ctx);
} FuzzTarget;
extern const FuzzTarget fuzz_target;
#define FUZZ_TARGET(n) const FuzzTarget fuzz_target = {#n, fuzz_##n, fuzz_##n##_seeds}

/* Scratch directory for targets that need real files (default build/fuzz/work, FUZZ_WORKDIR overrides); created on first use. */
const char *fuzz_workdir(void);
/* Little-endian writers for building seeds. */
static inline void fz_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static inline void fz_put32(uint8_t *p, uint32_t v) { fz_put16(p, (uint16_t)v), fz_put16(p + 2, (uint16_t)(v >> 16)); }
static inline void fz_put64(uint8_t *p, uint64_t v) { fz_put32(p, (uint32_t)v), fz_put32(p + 4, (uint32_t)(v >> 32)); }
/* Byte reader over the fuzz input: out-of-range reads return 0 (inputs of any length are valid). */
typedef struct {
    const uint8_t *p;
    size_t n, at;
} FzIn;
static inline uint8_t fz_u8(FzIn *r) { return r->at < r->n ? r->p[r->at++] : (r->at++, 0); }
static inline uint16_t fz_u16(FzIn *r) {
    uint16_t v = fz_u8(r);
    return (uint16_t)(v | (uint16_t)fz_u8(r) << 8);
}
static inline uint32_t fz_u32(FzIn *r) {
    uint32_t v = fz_u16(r);
    return v | (uint32_t)fz_u16(r) << 16;
}
static inline size_t fz_left(const FzIn *r) { return r->at < r->n ? r->n - r->at : 0; }
#endif
