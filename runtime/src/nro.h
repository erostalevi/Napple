/* NRO / NRR formats and the checks the ro system module applies before it maps a module (pure; used by ro.c, tested in runtime/tests/host).
 * Results are ro's (module 22). Signatures of an NRR are not verified (no console keys): its program id, kind and hash list are. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RO_RESULT(d) (22u | ((uint32_t)(d) << 9))
#define R_RO_OUT_OF_ADDRESS_SPACE RO_RESULT(2)
#define R_RO_ALREADY_LOADED RO_RESULT(3)
#define R_RO_INVALID_NRO RO_RESULT(4)
#define R_RO_INVALID_NRR RO_RESULT(6)
#define R_RO_TOO_MANY_NRO RO_RESULT(7)
#define R_RO_TOO_MANY_NRR RO_RESULT(8)
#define R_RO_NOT_AUTHORIZED RO_RESULT(9)
#define R_RO_INVALID_NRR_KIND RO_RESULT(10)
#define R_RO_INTERNAL_ERROR RO_RESULT(1023)
#define R_RO_INVALID_ADDRESS RO_RESULT(1025)
#define R_RO_INVALID_SIZE RO_RESULT(1026)
#define R_RO_NOT_LOADED RO_RESULT(1028)
#define R_RO_NOT_REGISTERED RO_RESULT(1029)
#define R_RO_INVALID_SESSION RO_RESULT(1030)
#define R_RO_INVALID_PROCESS RO_RESULT(1031)

#define NRO_HEADER_SIZE 0x80
typedef struct {
    uint32_t text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size, size;
    uint8_t module_id[32];
} NroInfo;
/* NRO header of `image` (nro_size bytes as the guest passed them) checked like ro's ValidateNro: magic, size and bss size as passed,
 * page-aligned sections, text at 0 and text/ro/data contiguous to the end of the file. */
uint32_t nro_validate(const uint8_t *image, uint64_t nro_size, uint64_t bss_size, NroInfo *out);

enum { NRR_KIND_USER = 0, NRR_KIND_JIT_PLUGIN = 1 };
#define NRR_HEADER_SIZE 0x350
typedef struct {
    uint64_t program_id;
    uint32_t size;
    uint8_t kind;
    uint32_t hashes_off, nhashes;
} NrrInfo;
/* NRR at `nrr` (size bytes). program_id 0 skips the program id check; expect_kind < 0 skips the kind check. */
uint32_t nrr_validate(const uint8_t *nrr, uint64_t size, uint64_t program_id, int expect_kind, NrrInfo *out);
/* 1 when sha256 `hash` is in the NRR's (sorted) hash list. */
int nrr_has_hash(const uint8_t *nrr, const NrrInfo *info, const uint8_t hash[32]);

void nro_id_hex(const uint8_t id[32], unsigned bytes, char *out); /* lowercase hex of the first `bytes` bytes */
