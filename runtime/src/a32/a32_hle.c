/* a32_hle.c - host functions behind AArch32 imports.
 *
 * The SDK's NVN driver is statically linked into the title; as for 64-bit titles it is never executed: nvnBootstrapLoader is overridden by the host
 * and returns, for every NVN entry point name, the address of a stub the interpreter traps. The trap lands here: a32_hle_call reads the arguments
 * under the 32-bit AAPCS (hard float: floats in s0.., 64-bit values in even register pairs, the rest on the stack), converts guest pointers to host
 * pointers (+ window base), calls the host function with a generic register-only prototype (every NVN entry point takes at most 8 integer-class
 * arguments and 8 floats, so the host's ABI places them in x0..x7 and s0..s7 whatever the real signature), and returns the result in r0/r1/s0
 * (host pointers back to guest addresses). Signatures come from gfx/src/nvnapi/nvn_protos.h (tools/gen_a32_nvn.py -> a32_nvn_sigs.inc). */
#include "napple.h"
#include "a32.h"
#include "a32_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern A32Window g_a32w;
extern uint8_t *memory_a32_base(void);
void *nvn_lookup(const char *name); /* libnapple_gfx: implemented function, unimplemented thunk, or NULL */

typedef struct {
    const char *name;
    char ret;
    const char *args;
} NvnSig;
static const NvnSig nvn_sigs[] = {
#include "a32_nvn_sigs.inc"
};
#define NSIGS (sizeof nvn_sigs / sizeof *nvn_sigs)

typedef uint64_t (*GenFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, float, float, float, float, float, float, float,
                          float);
typedef float (*GenFnF)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, float, float, float, float, float, float, float, float);

/* per stub (indexed like the window's hle table): the host function and signature of an NVN entry point */
static struct {
    void *fn;
    const NvnSig *sig;
    const char *name;
} nvn_stub[4096];

static const NvnSig *find_sig(const char *name) {
    for (size_t i = 0; i < NSIGS; i++)
        if (!strcmp(nvn_sigs[i].name, name)) return &nvn_sigs[i];
    return NULL;
}
/* the guest address of the stub for NVN entry point `name` (0 if the host does not know it) */
static uint32_t nvn_entry(const char *name) {
    void *fn = nvn_lookup(name);
    const NvnSig *sig = find_sig(name);
    char label[200];
    snprintf(label, sizeof label, "nvn:%s", name);
    uint32_t addr = a32_hle_stub(&g_a32w, label);
    unsigned i = (addr - A32_HLE_BASE) / 4;
    if (!fn ||
        !sig) { /* an entry point the host does not implement: the guest gets a stub that does nothing and returns 0, not NULL (it would treat NULL as "no such function" and abort or crash later) */
        LOG("a32", "nvn: no host implementation or signature for %s: stub returns 0", name);
        if (i < sizeof nvn_stub / sizeof *nvn_stub) {
            nvn_stub[i].fn = NULL;
            nvn_stub[i].sig = NULL;
            nvn_stub[i].name = "<unknown>";
        }
        return addr;
    }
    if (i < sizeof nvn_stub / sizeof *nvn_stub) {
        nvn_stub[i].fn = fn;
        nvn_stub[i].sig = sig;
        nvn_stub[i].name = sig->name;
    }
    return addr;
}

int a32_is_host_override(const char *name) { return !strcmp(name, "nvnBootstrapLoader"); }

static inline uint32_t stack_u32(A32Cpu *c, uint32_t off) {
    uint32_t v;
    memcpy(&v, c->mem + c->r[13] + off, 4);
    return v;
}

int a32_hle_call(A32Cpu *c, const char *name, uint32_t pc) {
    uint8_t *B = memory_a32_base();
    if (!strcmp(name, "nvnBootstrapLoader")) {
        const char *n = c->r[0] > 0x10000 ? (const char *)(B + c->r[0]) : NULL;
        c->r[0] = n ? nvn_entry(n) : 0;
        return 0;
    }
    if (strncmp(name, "nvn:", 4)) return 1;
    unsigned idx = (pc - A32_HLE_BASE) / 4;
    if (idx >= sizeof nvn_stub / sizeof *nvn_stub) return 1;
    if (!nvn_stub[idx].fn) {
        static int once[4096];
        if (!once[idx]++) LOG("a32", "nvn: call to unimplemented %s ignored (returns 0)", name);
        c->r[0] = 0;
        c->r[1] = 0;
        return 0;
    }
    const NvnSig *sg = nvn_stub[idx].sig;
    {
        static int tr = -1;
        if (tr < 0) tr = getenv("NAPPLE_A32_NVNTRACE") != NULL;
        if (tr) LOG("a32", "nvn call %s(r0=%#x r1=%#x r2=%#x r3=%#x) lr %#x", sg->name, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
    }
    if (!strcmp(sg->name, "nvnDeviceGetProcAddress")) {
        c->r[0] = c->r[1] > 0x10000 ? nvn_entry((const char *)(B + c->r[1])) : 0;
        return 0;
    }
    uint64_t xi[8] = {0};
    float fv[8] = {0};
    unsigned nx = 0, nf = 0, ncrn = 0, nfp = 0;
    uint32_t sp_off = 0;
    for (const char *a = sg->args; *a; a++) {
        char k = *a;
        if (k == 'q') {
            uint64_t v;
            if (ncrn & 1) ncrn++;
            if (ncrn < 4) {
                v = (uint64_t)c->r[ncrn] | (uint64_t)c->r[ncrn + 1] << 32;
                ncrn += 2;
            } else {
                ncrn = 4;
                sp_off = (sp_off + 7) & ~7u;
                v = (uint64_t)stack_u32(c, sp_off) | (uint64_t)stack_u32(c, sp_off + 4) << 32;
                sp_off += 8;
            }
            xi[nx++] = v;
        } else if (k == 'f') {
            uint32_t bits;
            if (nfp < 16) bits = VS(c, nfp++);
            else {
                bits = stack_u32(c, sp_off);
                sp_off += 4;
            }
            memcpy(&fv[nf++], &bits, 4);
        } else {
            uint32_t v;
            if (ncrn < 4) v = c->r[ncrn++];
            else {
                v = stack_u32(c, sp_off);
                sp_off += 4;
            }
            uint64_t x = v;
            if (k == 'p') x = v ? (uint64_t)(uintptr_t)B + v : 0;
            else if (k == 'Z') x = (uint64_t)(int64_t)(int32_t)v;
            else if (k == 'c') x = 0;
            xi[nx++] = x;
        }
        if (nx > 8 || nf > 8) runtime_fail("a32 nvn: %s has too many arguments for the generic call", sg->name);
    }
    /* arguments whose layout differs between the ABIs: arrays of pointers and a struct with a pointer field (everything else in NVN has identical layouts) */
    uint64_t ptr_a[16], ptr_b[16];
    struct {
        uint64_t data, control;
    } shd[8];
    if (!strcmp(sg->name, "nvnWindowBuilderSetTextures") || !strcmp(sg->name, "nvnCommandBufferSetRenderTargets")) {
        int is_win = !strcmp(sg->name, "nvnWindowBuilderSetTextures");
        int32_t n = (int32_t)xi[1];
        uint64_t arr = is_win ? xi[2] : xi[2];
        if (n < 0 || n > 16) runtime_fail("a32 nvn: %s with %d targets", sg->name, n);
        if (arr) {
            const uint32_t *g = (const uint32_t *)(uintptr_t)arr;
            for (int i = 0; i < n; i++) ptr_a[i] = g[i] ? (uint64_t)(uintptr_t)B + g[i] : 0;
            xi[2] = (uint64_t)(uintptr_t)ptr_a;
        }
        if (!is_win && xi[3]) {
            const uint32_t *g = (const uint32_t *)(uintptr_t)xi[3];
            for (int i = 0; i < n; i++) ptr_b[i] = g[i] ? (uint64_t)(uintptr_t)B + g[i] : 0;
            xi[3] = (uint64_t)(uintptr_t)ptr_b;
        }
    } else if (
        !strcmp(
            sg->name,
            "nvnMemoryPoolMapVirtual")) { /* NVNmappingRequest {pool*, int64 physical offset, int64 virtual offset, int64 size, int storage class}: the 40-byte layouts agree except the pool pointer's width */
        int32_t n = (int32_t)xi[1];
        if (n < 0 || n > (1 << 16)) runtime_fail("a32 nvn: nvnMemoryPoolMapVirtual with %d requests", n);
        uint8_t *reqs = malloc((size_t)n * 40 + 8);
        if (xi[2]) {
            memcpy(reqs, (const void *)(uintptr_t)xi[2], (size_t)n * 40);
            for (int i = 0; i < n; i++) {
                uint32_t pool;
                memcpy(&pool, reqs + i * 40, 4);
                uint64_t hp = pool ? (uint64_t)(uintptr_t)B + pool : 0;
                memcpy(reqs + i * 40, &hp, 8);
            }
            xi[2] = (uint64_t)(uintptr_t)reqs;
        }
        uint64_t r = ((GenFn)nvn_stub[idx].fn)(xi[0], xi[1], xi[2], xi[3], xi[4], xi[5], xi[6], xi[7], fv[0], fv[1], fv[2], fv[3], fv[4], fv[5], fv[6], fv[7]);
        free(reqs);
        c->r[0] = (uint32_t)r;
        return 0;
    } else if (!strcmp(sg->name, "nvnProgramSetShaders")) {
        int32_t n = (int32_t)xi[1];
        if (n < 0 || n > 8) runtime_fail("a32 nvn: nvnProgramSetShaders with %d stages", n);
        if (xi[2]) {
            const uint32_t *g = (const uint32_t *)(uintptr_t)xi[2];
            for (int i = 0; i < n; i++) {
                uint32_t lo = g[i * 4], hi = g[i * 4 + 1], ctl = g[i * 4 + 2];
                shd[i].data = (uint64_t)lo | (uint64_t)hi << 32;
                shd[i].control = ctl ? (uint64_t)(uintptr_t)B + ctl : 0;
            }
            xi[2] = (uint64_t)(uintptr_t)shd;
        }
    }
    if (sg->ret == 'f') {
        float r = ((GenFnF)nvn_stub[idx].fn)(xi[0], xi[1], xi[2], xi[3], xi[4], xi[5], xi[6], xi[7], fv[0], fv[1], fv[2], fv[3], fv[4], fv[5], fv[6], fv[7]);
        memcpy(&VS(c, 0), &r, 4);
        return 0;
    }
    uint64_t r = ((GenFn)nvn_stub[idx].fn)(xi[0], xi[1], xi[2], xi[3], xi[4], xi[5], xi[6], xi[7], fv[0], fv[1], fv[2], fv[3], fv[4], fv[5], fv[6], fv[7]);
    switch (sg->ret) {
        case 'v': break;
        case 'p': c->r[0] = r >= (uintptr_t)B && r < (uintptr_t)B + (1ull << 32) ? (uint32_t)(r - (uintptr_t)B) : (uint32_t)r; break;
        case 'q':
            c->r[0] = (uint32_t)r;
            c->r[1] = (uint32_t)(r >> 32);
            break;
        default: c->r[0] = (uint32_t)r; break;
    }
    return 0;
}
