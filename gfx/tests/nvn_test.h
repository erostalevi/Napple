/* nvn_test.h - helpers for headless NVN tests: loader macro, tiny Maxwell assembler, shader blob builder. No game data. */
#ifndef NVN_TEST_H
#define NVN_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../include/napple_gfx.h"
#include "nvnapi/nvn_protos.h"

#define L(name) ((NVNT_##name)napple_gfx_nvn_bootstrap_loader(#name))
static int g_fail __attribute__((unused));
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define PT_BITS (7ull << 16)
static uint64_t ins_exit(void) { return 0xe300000000000000ull | 0xf | PT_BITS; }
static uint64_t ins_mov32i(int d, uint32_t imm) { return 0x0100000000000f00ull | (uint64_t)d | ((uint64_t)imm << 20) | PT_BITS; }
static uint64_t ins_ald(int d, int addr, int n) { return 0xefd8000000000000ull | (uint64_t)d | (255ull << 8) | ((uint64_t)addr << 20) | ((uint64_t)(n - 1) << 47) | PT_BITS; }
static uint64_t ins_ast(int s, int addr, int n) { return 0xeff0000000000000ull | (uint64_t)s | (255ull << 8) | ((uint64_t)addr << 20) | ((uint64_t)(n - 1) << 47) | PT_BITS; }

/* Build a shader blob: NVN header (0x30) + SPH (0x50) + instructions, then the control section the driver would have produced. */
static size_t build_shader(uint8_t *mem, uint8_t *ctl, int stage, const uint64_t *insns, int n, uint32_t omap_target) {
    memset(mem, 0, 0x1000); memset(ctl, 0, 0x880);
    uint32_t magic = 0x12345678; memcpy(mem, &magic, 4);
    memcpy(mem + 0x48 + 0x30, &omap_target, 4);                  /* SPH + 0x48: fragment color target mask */
    uint64_t *w = (uint64_t *)(mem + 0x80);
    int o = 0;
    w[o++] = 0;                                                  /* scheduling word */
    for (int i = 0; i < n; i++) { w[o++] = insns[i]; if (o % 4 == 0) w[o++] = 0; }
    while (o % 4) w[o++] = 0;
    uint32_t code_len = (uint32_t)(0x80 + o * 8 + 0x80);
    code_len = (code_len + 0xff) & ~0xffu;
    uint32_t zero = 0, total = code_len + 0x100, stg = (uint32_t)stage;
    memcpy(ctl + 0x6fc, &zero, 4); memcpy(ctl + 0x700, &code_len, 4); memcpy(ctl + 0x704, &total, 4); memcpy(ctl + 0x714, &stg, 4);
    return total;
}


static uint64_t ins_tex2d_lz(int d, int a, int b, int index) { return 0xc038000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | (1ull << 29) | (0xfull << 31) | ((uint64_t)index << 36) | (1ull << 55) | PT_BITS; }
#endif
