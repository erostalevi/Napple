/* NRO / NRR validation (see nro.h). Layouts follow the public descriptions of the formats (switchbrew, Atmosphère's ro module). */
#include "nro.h"
#include <stdio.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static int page_aligned(uint64_t v) { return (v & 0xfff) == 0; }

uint32_t nro_validate(const uint8_t *img, uint64_t nro_size, uint64_t bss_size, NroInfo *o) {
    memset(o, 0, sizeof *o);
    if (nro_size < NRO_HEADER_SIZE || memcmp(img + 0x10, "NRO0", 4)) return R_RO_INVALID_NRO;
    o->size = rd32(img + 0x18);
    o->text_off = rd32(img + 0x20);
    o->text_size = rd32(img + 0x24);
    o->ro_off = rd32(img + 0x28);
    o->ro_size = rd32(img + 0x2c);
    o->data_off = rd32(img + 0x30);
    o->data_size = rd32(img + 0x34);
    o->bss_size = rd32(img + 0x38);
    memcpy(o->module_id, img + 0x40, 32);
    if (o->size != nro_size || o->bss_size != bss_size) return R_RO_INVALID_NRO;
    if (!page_aligned(o->text_size) || !page_aligned(o->ro_size) || !page_aligned(o->data_size) || !page_aligned(o->bss_size)) return R_RO_INVALID_NRO;
    if (o->text_off > o->ro_off || o->ro_off > o->data_off) return R_RO_INVALID_NRO;
    if (o->text_off != 0 || (uint64_t)o->text_off + o->text_size != o->ro_off || (uint64_t)o->ro_off + o->ro_size != o->data_off ||
        (uint64_t)o->data_off + o->data_size != o->size)
        return R_RO_INVALID_NRO;
    return 0;
}

uint32_t nrr_validate(const uint8_t *nrr, uint64_t size, uint64_t program_id, int expect_kind, NrrInfo *o) {
    memset(o, 0, sizeof *o);
    if (size < NRR_HEADER_SIZE || memcmp(nrr, "NRR0", 4)) return R_RO_INVALID_NRR;
    o->program_id = rd64(nrr + 0x330);
    o->size = rd32(nrr + 0x338);
    o->kind = nrr[0x33c];
    o->hashes_off = rd32(nrr + 0x340);
    o->nhashes = rd32(nrr + 0x344);
    if (o->size != size) return R_RO_INVALID_SIZE;
    uint64_t mask = rd64(nrr + 0x10), pattern = rd64(nrr + 0x18);
    if (program_id && ((program_id & mask) != pattern || o->program_id != program_id)) return R_RO_INVALID_NRR;
    if (expect_kind >= 0 && o->kind != expect_kind) return R_RO_INVALID_NRR_KIND;
    if (o->hashes_off < NRR_HEADER_SIZE || o->hashes_off > size || (uint64_t)o->nhashes * 32 > size - o->hashes_off) return R_RO_INVALID_NRR;
    return 0;
}

int nrr_has_hash(const uint8_t *nrr, const NrrInfo *info, const uint8_t hash[32]) {
    uint32_t lo = 0, hi = info->nhashes; /* the list is sorted (ro looks hashes up by binary search) */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = memcmp(nrr + info->hashes_off + (uint64_t)mid * 32, hash, 32);
        if (!c) return 1;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

void nro_id_hex(const uint8_t id[32], unsigned bytes, char *out) {
    for (unsigned i = 0; i < bytes && i < 32; i++) snprintf(out + 2 * i, 3, "%02x", id[i]);
    out[2 * (bytes < 32 ? bytes : 32)] = 0;
}
