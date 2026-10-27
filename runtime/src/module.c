/* Module loader: maps the rewritten images of a game, applies their relocations from the tables file, resolves imports
 * across modules and against host overrides, and keeps the per-page permission state the SDK asks the kernel for.
 * Everything about a module (layout, symbols, relocations, svc gates) comes from <name>.tables (tools/prepare_tables.py
 * today, napple-rewrite later); nothing is compiled in per game. */
#define _GNU_SOURCE /* dladdr on non-Apple hosts (runtime/tests/host) */
#include "napple.h"
#include "json.h"
#include "sha256.h"
#include "ios_dylib.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if NAPPLE_IOS
#include <limits.h>
#include <mach-o/dyld.h>
#endif

Module *g_modules;
unsigned g_nmodules;

#define NTB_HEADER 152u /* bytes before the sym array: 24 + 48 + 48 + 32 */
static pthread_mutex_t perm_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *page_perm[MAX_MODULES];                 /* per module: one byte per 4 KiB data page: bits 0-2 permission, bit 4 locked */
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER; /* run-time loads and unloads (NROs) */
void dispatch_modules_changed(void);

static size_t page_up(size_t x) { return (x + PAGE_16K - 1) & ~(size_t)(PAGE_16K - 1); }
static unsigned fnv(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
const char *module_sym_name(const Module *m, unsigned i) { return m->strtab + m->syms[i].name; }

static void make_hash(Module *m) {
    unsigned cap = 16;
    while (cap < m->nsym * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof(unsigned));
    m->hash_mask = cap - 1;
    if (!m->hash) runtime_fail("symbol hash allocation");
    for (unsigned i = 1; i < m->nsym; i++) {
        if (!m->syms[i].defined) continue;
        const char *s = module_sym_name(m, i);
        unsigned p = fnv(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, module_sym_name(m, m->hash[p]))) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}
static void *module_symbol(const Module *m, const char *name) {
    unsigned p = fnv(name) & m->hash_mask;
    while (m->hash[p]) {
        const Sym *s = &m->syms[m->hash[p]];
        if (!strcmp(name, m->strtab + s->name)) return s->value < m->key_off ? m->base + s->value : NULL;
        p = (p + 1) & m->hash_mask;
    }
    return NULL;
}
void *module_find_symbol(const char *name) {
    for (unsigned i = 0; i < g_nmodules; i++) {
        if (g_modules[i].gone) continue;
        void *p = module_symbol(&g_modules[i], name);
        if (p) return p;
    }
    return NULL;
}
const Module *module_by_name(const char *name) {
    for (unsigned i = 0; i < g_nmodules; i++)
        if (!g_modules[i].gone && !strcmp(g_modules[i].name, name)) return &g_modules[i];
    return NULL;
}
const Module *module_for_address(uint64_t a) {
    for (unsigned i = 0; i < g_nmodules; i++) {
        uint64_t b = (uintptr_t)g_modules[i].base;
        if (b && !g_modules[i].gone && a >= b && a < b + g_modules[i].size) return &g_modules[i];
    }
    return NULL;
}
int module_pc_is_guest_code(uint64_t pc) {
    const Module *m = module_for_address(pc);
    return m && pc - (uintptr_t)m->base < m->text_end;
}
uint64_t module_image_bytes(void) {
    uint64_t total = 0;
    for (unsigned i = 0; i < g_nmodules; i++)
        if (!g_modules[i].gone) total += g_modules[i].key_off;
    return page_up(total);
}
void module_describe(uint64_t a, char *out, size_t n) {
    const Module *m = module_for_address(a);
    if (m) {
        snprintf(out, n, "%s+0x%llx", m->name, (unsigned long long)(a - (uintptr_t)m->base));
        return;
    }
    Dl_info di;
    if (dladdr((void *)(uintptr_t)a, &di) && di.dli_sname) snprintf(out, n, "%s+0x%llx", di.dli_sname, (unsigned long long)(a - (uintptr_t)di.dli_saddr));
    else snprintf(out, n, "0x%llx", (unsigned long long)a);
}

/* ---- host symbol overrides ----------------------------------------------------------------------------------- */
static struct {
    const HostSymbol *table;
    size_t count;
} host_tables[16];
static unsigned nhost_tables;
void host_symbols_register(const HostSymbol *table, size_t count) {
    if (nhost_tables == 16) runtime_fail("too many host symbol tables");
    host_tables[nhost_tables].table = table;
    host_tables[nhost_tables++].count = count;
}
void *host_symbol_lookup(const char *name) {
    for (unsigned t = 0; t < nhost_tables; t++)
        for (size_t i = 0; i < host_tables[t].count; i++)
            if (!strcmp(host_tables[t].table[i].name, name)) return host_tables[t].table[i].fn;
    return NULL;
}

/* Code hooks (manifest "hooks": [{"module","offset","symbol"}]): a stripped module that carries its own copy of a function the host replaces by name (a firmware
 * applet's statically linked NVN driver has no exported nvnBootstrapLoader) gets its first instructions replaced by a jump to the host symbol. `napple build` finds
 * the offsets by signature (fwprog.rs). */
static struct {
    char module[16], symbol[64];
    uint64_t offset;
} hooks[8];
static unsigned nhooks;
/* Known-build patches (manifest "patches": [{"module","offset","word"}]): single instruction words the build writes for a firmware program whose behaviour is known to need it
 * (fwprog.rs keeps the list, keyed by the main module's build id). */
static struct {
    char module[16];
    uint64_t offset;
    uint32_t word;
} patches[16];
static unsigned npatches;
static struct {
    uintptr_t addr;
    Module *module;
    unsigned long off;
    uint32_t orig;
} dbglog[64];
static unsigned ndbglog;
/* SIGTRAP from a NAPPLE_DEBUG_LOG point: log, restore the instruction, resume. Returns 1 when pc was one of them. */
int module_debug_log_hit(uint64_t pc, const uint64_t *x, uint64_t lr, uint64_t tid) {
    for (unsigned i = 0; i < ndbglog; i++) {
        if (dbglog[i].addr != pc) continue;
        char where[96];
        module_describe(lr, where, sizeof where);
        LOG("trace", "%s+%#lx first hit: thread %#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx lr=%s", dbglog[i].module->name, dbglog[i].off, (unsigned long long)tid, (unsigned long long)x[0],
            (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3], where);
        if (getenv("NAPPLE_DEBUG_LOG_MEM")) { /* NAPPLE_DEBUG_LOG_MEM=1: also the 0x60 bytes behind x0..x3 when readable */
            for (int r = 0; r < 4; r++) {
                uint8_t buf[0x60];
                mach_vm_size_t got = 0;
                if (x[r] < 0x100000000ull || mach_vm_read_overwrite(mach_task_self(), x[r], sizeof buf, (mach_vm_address_t)(uintptr_t)buf, &got) != KERN_SUCCESS || got != sizeof buf) continue;
                char hex[0xc0 + 1];
                for (unsigned k = 0; k < sizeof buf; k++) snprintf(hex + 2 * k, 3, "%02x", buf[k]);
                LOG("trace", "  *x%d = %s", r, hex);
            }
        }
        uintptr_t page = pc & ~(uintptr_t)(PAGE_16K - 1);
        mprotect((void *)page, PAGE_16K, PROT_READ | PROT_WRITE);
        memcpy((void *)pc, &dbglog[i].orig, 4);
        mprotect((void *)page, PAGE_16K, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)pc, (char *)pc + 4);
        dbglog[i].addr = 0;
        return 1;
    }
    return 0;
}

/* ---- loading ------------------------------------------------------------------------------------------------- */
static void *read_whole(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    void *buf = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, (char *)buf + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }
    ((char *)buf)[got] = 0;
    *size = got;
    return buf;
}

static void hex(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* <dir>/<file>.tables; file is the module name, or the module id of an NRO */
static void load_tables(Module *m, const char *dir, const char *file) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.tables", dir, file);
    size_t size;
    uint8_t *d = read_whole(path, &size);
    if (!d || size < NTB_HEADER || memcmp(d, "NTB1", 4)) runtime_fail("module tables %s missing or not NTB1", path);
    uint32_t u32[6];
    memcpy(u32, d, sizeof u32);
    m->nsym = u32[2];
    m->nrel = u32[3];
    m->ngate = u32[4];
    uint32_t strsz = u32[5];
    uint64_t h[12];
    memcpy(h, d + 24, sizeof h);
    m->file_size = h[0];
    m->text_end = h[1];
    m->ro_off = h[2];
    m->data_off = h[3];
    m->patch_off = h[4];
    m->key_off = h[5];
    m->dt_init = h[6];
    m->dt_fini = h[7];
    m->init_array = h[8];
    m->init_array_size = h[9];
    m->fini_array = h[10];
    m->fini_array_size = h[11];
    memcpy(m->build_id, d + 120, 32);
    size_t need = NTB_HEADER + (size_t)(m->nsym + m->nrel + m->ngate) * 24 + strsz;
    if (need != size) runtime_fail("module tables %s: size %zu, expected %zu", path, size, need);
    m->tables_blob = d;
    m->syms = (const Sym *)(d + NTB_HEADER);
    m->rels = (const Reloc *)(m->syms + m->nsym);
    m->gates = (const Gate *)(m->rels + m->nrel);
    m->strtab = (const char *)(m->gates + m->ngate);
    if (m->nsym == 0 || m->strtab[strsz - 1] != 0) runtime_fail("module tables %s: malformed", path);
    for (unsigned i = 0; i < m->nsym; i++)
        if (m->syms[i].name >= strsz) runtime_fail("module tables %s: bad symbol name", path);
}

/* Layout check and the key page: shared by the anonymous-memory path (macOS) and the dylib path (iOS). */
static void finish_image(Module *m) {
    if (m->key_off % PAGE_16K || m->key_off + PAGE_16K > m->patch_off || m->patch_off > m->size) runtime_fail("%s: key page / patch layout", m->name);
    uint64_t *key = (uint64_t *)(m->base + m->key_off);
    for (unsigned i = 0; i < 8; i++) key[i] = (uint64_t)runtime_keys[i] * 8;
    make_hash(m);
}

#if NAPPLE_IOS
/* iOS cannot make anonymous memory executable (no JIT), so the executable pages of a module come from a code-signed Mach-O dylib that
 * `napple build --target ios` wrote at build time: the same image at +IOS_DYLIB_HEADER_PAGE, with the segment protections seal() would
 * set on macOS already in place (crates/napple-build/src/macho.rs). dyld maps it; the key page and the relocations are written into its
 * private, writable __DATA segment exactly as on macOS. */
static const void *find_image_header(const char *path) {
    char want[PATH_MAX], have[PATH_MAX];
    if (!realpath(path, want)) snprintf(want, sizeof want, "%s", path);
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (n && (!strcmp(n, path) || !strcmp(n, want) || (realpath(n, have) && !strcmp(have, want)))) return _dyld_get_image_header(i);
    }
    return NULL;
}
static void map_dylib(Module *m, const char *dir, const char *file) {
    char path[4096];
    if (ios_dylib_path(path, sizeof path, dir, file)) runtime_fail("%s: cannot derive the module dylib path from %s", m->name, dir);
    m->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m->dl)
        runtime_fail(
            "cannot load %s: %s (the dylib must be code-signed with the app's identity; rebuild with napple build --target ios and a valid provisioning profile)",
            path, dlerror());
    const void *hdr = find_image_header(path);
    if (!hdr) runtime_fail("%s: loaded, but dyld does not list it", path);
    const char *bad = ios_dylib_check(hdr, m->text_end, m->data_off, m->key_off, m->patch_off, m->file_size);
    if (bad) runtime_fail("%s: %s", path, bad);
    m->base = (uint8_t *)hdr + IOS_DYLIB_HEADER_PAGE;
    m->size = page_up(m->file_size);
    finish_image(m);
}
#endif

static void map_image(Module *m, const char *dir, const char *file, const char *expect_sha) {
#if NAPPLE_IOS
    (void)expect_sha; /* the code signature covers the dylib */
    map_dylib(m, dir, file);
    return;
#else
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.patched.bin", dir, file);
    int fd = open(path, O_RDONLY);
    if (fd < 0) runtime_fail("cannot open %s", path);
    struct stat st;
    if (fstat(fd, &st) || (uint64_t)st.st_size != m->file_size) runtime_fail("%s: size differs from its tables", path);
    m->size = page_up(m->file_size);
    /* Horizon maps modules at 4 KiB granularity, the host protects 16 KiB pages. A module small enough that text and data_off share one host page (MiiEdit's
     * 8 KiB `sdk` stub) is loaded 4/8/12 KiB past a host page boundary, so its data segment starts a page of its own: adrp and every offset stay valid, only the
     * absolute alignment of the module changes. */
    m->skew = 0;
    if (page_up(m->text_end) > (m->data_off & ~(uint64_t)(PAGE_16K - 1)) && m->data_off % PAGE_16K) m->skew = PAGE_16K - m->data_off % PAGE_16K;
    uint8_t *map = mmap(NULL, page_up(m->file_size + m->skew), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) runtime_fail("cannot map %s", m->name);
    m->base = map + m->skew;
    for (size_t got = 0; got < m->file_size;) {
        ssize_t n = pread(fd, m->base + got, m->file_size - got, (off_t)got);
        if (n <= 0) runtime_fail("short read of %s", path);
        got += (size_t)n;
    }
    close(fd);
    if (expect_sha && *expect_sha && !getenv("NAPPLE_SKIP_VERIFY")) {
        uint8_t digest[32];
        char text[65];
        sha256(m->base, m->file_size, digest);
        hex(digest, 32, text);
        if (strcmp(text, expect_sha)) runtime_fail("%s: image sha256 differs from the manifest", path);
    }
    finish_image(m);
#endif
}

/* Imports the loader could not bind: called through a per-import trap stub (hle_stack.c) so the failure names it. */
static unsigned unresolved_count;
extern void *runtime_hle_wrap_trap(const char *name);

static void relocate(Module *m) {
    for (unsigned i = 0; i < m->nrel; i++) {
        const Reloc *r = &m->rels[i];
        if (r->where < m->data_off || r->where + 8 > m->key_off || r->symbol >= m->nsym) runtime_fail("%s: relocation outside data", m->name);
        uint64_t v;
        if (r->kind == 1027) {
            if (r->addend < 0 || (uint64_t)r->addend >= m->key_off) runtime_fail("%s: relative addend", m->name);
            v = (uintptr_t)m->base + (uint64_t)r->addend;
            m->relative++;
        } else {
            if (r->kind != 257 && r->kind != 1025 && r->kind != 1026) runtime_fail("%s: relocation kind %u", m->name, r->kind);
            const Sym *s = &m->syms[r->symbol];
            const char *name = m->strtab + s->name;
            void *p = host_symbol_lookup(name);
            if (p) {
                LOGV("loader", "%s: %s bound to a host override", m->name, name);
                p = runtime_hle_wrap_named(p, name);
            }
            if (!p) p = s->defined ? (s->value < m->key_off ? (void *)(m->base + s->value) : NULL) : module_find_symbol(name);
            if (!p && s->binding != 2) { /* strong undefined: bind to a trap that names the import when called */
                p = runtime_hle_wrap_trap(name);
                unresolved_count++;
                trace_event("unresolved_import", "\"module\":\"%s\",\"symbol\":\"%s\"", m->name, name);
            }
            if (!p) m->weak++;
            if (p && !s->defined) m->cross++;
            v = (uintptr_t)p + (r->kind == 1026 ? 0 : (uint64_t)r->addend);
            m->symbolic++;
        }
        memcpy(m->base + r->where, &v, 8);
    }
}

static void seal(Module *m) {
#if NAPPLE_IOS
    /* text, rodata and the patch area already have their final protections from the dylib's segments; only the key page turns read-only. */
    if (mprotect(m->base + m->key_off, PAGE_16K, PROT_READ)) runtime_fail("%s: key page R: %s", m->name, strerror(errno));
    return;
#else
    /* Debug aid: NAPPLE_DEBUG_TRAP="main+0x2259f8,sdk+0x40" replaces those instructions by brk #0, so the crash report shows the registers where the guest reaches them. */
    for (const char *t = getenv("NAPPLE_DEBUG_TRAP"); t && *t;) {
        char nm[16]; unsigned long off; int used = 0;
        if (sscanf(t, "%15[^+]+%lx%n", nm, &off, &used) < 2 || !used) break;
        if (!strcmp(nm, m->name) && off + 4 <= m->text_end) { uint32_t brk = 0xD4200000u; memcpy(m->base + off, &brk, 4); LOG("loader", "debug trap at %s+%#lx", nm, off); }
        t += used; if (*t == ',') t++; else break;
    }
    /* Debug aid: NAPPLE_DEBUG_LOG="main+0x60d20,main+0x7c80" logs the first hit of each (thread, x0..x3, lr) and then lets the guest go on (the instruction is put back). */
    for (const char *t = getenv("NAPPLE_DEBUG_LOG"); t && *t;) {
        char nm[16];
        unsigned long off;
        int used = 0;
        if (sscanf(t, "%15[^+]+%lx%n", nm, &off, &used) < 2 || !used) break;
        if (!strcmp(nm, m->name) && off + 4 <= m->text_end && ndbglog < 64) {
            dbglog[ndbglog].addr = (uintptr_t)m->base + off;
            dbglog[ndbglog].module = m;
            dbglog[ndbglog].off = off;
            memcpy(&dbglog[ndbglog++].orig, m->base + off, 4);
            uint32_t brk = 0xD4200200u; /* brk #0x10 */
            memcpy(m->base + off, &brk, 4);
        }
        t += used;
        if (*t == ',') t++;
        else break;
    }
    for (unsigned h = 0; h < nhooks; h++) {
        if (strcmp(hooks[h].module, m->name)) continue;
        void *fn = host_symbol_lookup(hooks[h].symbol);
        if (!fn || hooks[h].offset + 16 > m->text_end) {
            LOG("loader", "hook %s+%#llx -> %s skipped (%s)", m->name, (unsigned long long)hooks[h].offset, hooks[h].symbol, fn ? "outside the text" : "no host symbol");
            continue;
        }
        uint32_t code[2] = {0x58000050u, 0xD61F0200u}; /* ldr x16, #8 ; br x16 ; .quad target */
        memcpy(m->base + hooks[h].offset, code, 8);
        memcpy(m->base + hooks[h].offset + 8, &fn, 8);
        LOG("loader", "hook: %s+%#llx now jumps to the host's %s", m->name, (unsigned long long)hooks[h].offset, hooks[h].symbol);
    }
    /* Debug aid: NAPPLE_DEBUG_PATCH="main+0x7fe0=52800020,main+0x7fe4=d65f03c0" overwrites instruction words (hex) before the text is sealed. */
    for (const char *t = getenv("NAPPLE_DEBUG_PATCH"); t && *t;) {
        char nm[16];
        unsigned long off, word;
        int used = 0;
        if (sscanf(t, "%15[^+]+%lx=%lx%n", nm, &off, &word, &used) < 3 || !used) break;
        if (!strcmp(nm, m->name) && off + 4 <= m->text_end) {
            uint32_t w = (uint32_t)word;
            memcpy(m->base + off, &w, 4);
            LOG("loader", "debug patch at %s+%#lx = %08x", nm, off, w);
        }
        t += used;
        if (*t == ',') t++;
        else break;
    }
    for (unsigned k = 0; k < npatches && !getenv("NAPPLE_NO_MANIFEST_PATCHES"); k++) {
        if (strcmp(patches[k].module, m->name) || patches[k].offset + 4 > m->text_end) continue;
        memcpy(m->base + patches[k].offset, &patches[k].word, 4);
        LOG("loader", "patch: %s+%#llx = %08x", m->name, (unsigned long long)patches[k].offset, patches[k].word);
    }
    uint8_t *map = m->base - m->skew;
    size_t rx = page_up(m->skew + m->text_end), ro_end = (m->skew + m->data_off) & ~(size_t)(PAGE_16K - 1);   /* offsets from the start of the mapping */
    if (rx > ro_end) runtime_fail("%s: text overlaps data", m->name);
    if (m->skew) LOG("loader", "%s: 4 KiB-aligned segments share a host page: mapped %#zx bytes into its mapping (data starts a page of its own)", m->name, m->skew);
    if (mprotect(map, rx, PROT_READ | PROT_EXEC)) runtime_fail("%s: text RX: %s", m->name, strerror(errno));
    if (ro_end > rx && mprotect(map + rx, ro_end - rx, PROT_READ)) runtime_fail("%s: rodata R", m->name);
    if (m->skew && m->patch_off < m->size) runtime_fail("%s: a skewed module cannot carry a patch area", m->name);
    if (!m->skew && mprotect(m->base + m->key_off, PAGE_16K, PROT_READ)) runtime_fail("%s: key page R", m->name);   /* skewed: the key page is not host-page aligned, left writable */
    if (m->patch_off < m->size && mprotect(m->base + m->patch_off, m->size - m->patch_off, PROT_READ | PROT_EXEC)) runtime_fail("%s: patch RX", m->name);
    __builtin___clear_cache((char *)m->base, (char *)m->base + rx);
    __builtin___clear_cache((char *)m->base + m->patch_off, (char *)m->base + m->size);
#endif
}

void modules_load(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    size_t size;
    char *text = read_whole(path, &size);
    if (!text) runtime_fail("cannot read %s", path);
    JsonNode *root = json_parse(text, size);
    const JsonNode *order = json_get(root, "load_order"), *mods = json_get(root, "modules");
    if (!root || !order || order->type != J_ARR || !mods) runtime_fail("%s: no load_order", path);
    unsigned n = 0;
    for (const JsonNode *o = order->child; o; o = o->next) n++;
    if (!n || n > MAX_MODULES / 2) runtime_fail("%s: unreasonable module count %u", path, n);
    g_modules = calloc(MAX_MODULES, sizeof *g_modules);
    g_nmodules = n;
    if (!g_modules) runtime_fail("module table");
    unsigned i = 0;
    for (const JsonNode *o = order->child; o; o = o->next, i++) {
        Module *m = &g_modules[i];
        if (o->type != J_STR || strlen(o->str) > 15 || strchr(o->str, '/')) runtime_fail("%s: bad module name", path);
        snprintf(m->name, sizeof m->name, "%s", o->str);
        const char *sha = NULL;
        for (const JsonNode *e = mods->child; e; e = e->next)
            if (!strcmp(json_str(e, "name", ""), m->name)) sha = json_str(e, "sha256", NULL);
        char mdir[4096];
        snprintf(mdir, sizeof mdir, "%s/modules", dir);
        load_tables(m, mdir, m->name);
        map_image(m, mdir, m->name, sha);
        char id[65];
        hex(m->build_id, 20, id);
        LOG("loader", "mapped %-8s %p size=%#zx symbols=%u relocs=%u gates=%u build=%.16s", m->name, m->base, m->size, m->nsym, m->nrel, m->ngate, id);
    }
    const JsonNode *hk = json_get(root, "hooks");
    for (const JsonNode *e = hk && hk->type == J_ARR ? hk->child : NULL; e && nhooks < 8; e = e->next) {
        snprintf(hooks[nhooks].module, sizeof hooks[0].module, "%s", json_str(e, "module", ""));
        snprintf(hooks[nhooks].symbol, sizeof hooks[0].symbol, "%s", json_str(e, "symbol", ""));
        hooks[nhooks++].offset = (uint64_t)json_num(e, "offset", 0);
    }
    const JsonNode *pt = json_get(root, "patches");
    for (const JsonNode *e = pt && pt->type == J_ARR ? pt->child : NULL; e && npatches < 16; e = e->next) {
        snprintf(patches[npatches].module, sizeof patches[0].module, "%s", json_str(e, "module", ""));
        patches[npatches].offset = (uint64_t)json_num(e, "offset", 0);
        patches[npatches++].word = (uint32_t)json_num(e, "word", 0);
    }
    json_free(root);
    free(text);
    for (i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
    for (i = 0; i < g_nmodules; i++) seal(&g_modules[i]);
    for (i = 0; i < g_nmodules; i++) {
        const Module *m = &g_modules[i];
        LOG("loader", "relocated %-8s relative=%u symbolic=%u cross-module=%u weak-null=%u", m->name, m->relative, m->symbolic, m->cross, m->weak);
    }
    LOG("loader", "%u imports bound to trap stubs (calling one stops the runtime with its name)", unresolved_count);
    summary_stage("loaded");
}

/* ---- run-time modules (NROs, ro.c) --------------------------------------------------------------------------------
 * The guest hands ldr:ro the NRO bytes; what runs is the image `napple build` rewrote ahead of time from the same file (found by module id),
 * mapped at a fresh host address like the static modules. Its imports bind like theirs (host overrides first, then every loaded module in
 * load order); the guest's own nn::ro relocation pass runs over the mapped image afterwards (see docs/architecture/runtime-services.md). */
Module *module_load_nro(const char *data_dir, const uint8_t module_id[32], const char **why) {
    char id[65], dir[4096], probe[4200];
    hex(module_id, 32, id);
    snprintf(dir, sizeof dir, "%s/nro", data_dir);
    snprintf(probe, sizeof probe, "%s/%s.tables", dir, id);
    if (access(probe, R_OK)) {
        *why = "no ahead-of-time rewrite of this NRO in the data directory (rebuild the game with napple build)";
        return NULL;
    }
    pthread_mutex_lock(&load_lock);
    unsigned slot = g_nmodules;
    for (unsigned i = 0; i < g_nmodules; i++)
        if (g_modules[i].gone) {
            slot = i;
            break;
        }
    if (slot == MAX_MODULES) {
        pthread_mutex_unlock(&load_lock);
        *why = "too many modules";
        return NULL;
    }
    Module *m = &g_modules[slot];
    Module fresh = {0};
    fresh.gone = 1;
    fresh.dynamic = 1;
    snprintf(fresh.name, sizeof fresh.name, "nro-%.11s", id);
    *m = fresh; /* readers skip it while gone is set */
    load_tables(m, dir, id);
    if (memcmp(m->build_id, module_id, 32)) runtime_fail("%s/%s.tables: build id differs from the NRO's module id", dir, id);
    map_image(m, dir, id, NULL);
    relocate(m);
    seal(m);
    __atomic_store_n(&m->gone, 0, __ATOMIC_RELEASE);
    if (slot == g_nmodules) __atomic_store_n(&g_nmodules, g_nmodules + 1, __ATOMIC_RELEASE);
    dispatch_modules_changed();
    pthread_mutex_unlock(&load_lock);
    LOG("loader", "NRO %s mapped at %p size=%#zx relative=%u symbolic=%u cross-module=%u gates=%u", m->name, m->base, m->size, m->relative, m->symbolic,
        m->cross, m->ngate);
    return m;
}
void module_unload(Module *m) {
    pthread_mutex_lock(&load_lock);
    if (!m->dynamic || m->gone) {
        pthread_mutex_unlock(&load_lock);
        return;
    }
    __atomic_store_n(&m->gone, 1, __ATOMIC_RELEASE);
    dispatch_modules_changed();
    unsigned i = (unsigned)(m - g_modules);
    pthread_mutex_lock(&perm_lock);
    free(page_perm[i]);
    page_perm[i] = NULL;
    pthread_mutex_unlock(&perm_lock);
#if NAPPLE_IOS
    if (m->dl) {
        dlclose(m->dl);
        m->dl = NULL;
    }
#else
    munmap(m->base - m->skew, page_up(m->size + m->skew));
#endif
    free(m->hash);
    m->hash = NULL; /* tables_blob stays: replaced gate indexes may still point into it */
    LOG("loader", "NRO %s unmapped", m->name);
    pthread_mutex_unlock(&load_lock);
}

/* ---- guest permission state over module data (svc 0x02 / 0x03 / 0x06) ------------------------------------------ */
static unsigned data_page_perm(unsigned i, uint64_t off) {
    const Module *m = &g_modules[i];
    if (off < m->data_off) return off < page_up(m->text_end) ? 5 : 1;
    if (off >= m->key_off) return off >= m->patch_off ? 5 : 1;
    return page_perm[i] ? page_perm[i][(off - m->data_off) / PAGE_4K] : 3;
}
static int in_data(const Module *m, uint64_t address, uint64_t size) {
    uint64_t base = (uintptr_t)m->base;
    return address >= base && address - base >= m->data_off && address - base < m->key_off && size <= m->key_off - (address - base);
}
static void ensure_perm(unsigned i) {
    if (page_perm[i]) return;
    size_t count = (g_modules[i].key_off - g_modules[i].data_off + PAGE_4K - 1) / PAGE_4K;
    page_perm[i] = malloc(count);
    if (!page_perm[i]) runtime_fail("module permission table");
    memset(page_perm[i], 3, count);
}
uint64_t module_set_permission(uint64_t address, uint64_t size, unsigned permission) {
    if (address & 4095) return R_INVALID_ADDRESS;
    if (!size || (size & 4095)) return R_INVALID_SIZE;
    if (permission != 0 && permission != 1 && permission != 3) return R_INVALID_MEMORY_PERMISSIONS;
    for (unsigned i = 0; i < g_nmodules; i++) {
        if (g_modules[i].gone) continue;
        Module *m = &g_modules[i];
        if (!in_data(m, address, size)) continue;
        uint64_t off = address - (uintptr_t)m->base;
        pthread_mutex_lock(&perm_lock);
        ensure_perm(i);
        for (uint64_t p = 0; p < size / PAGE_4K; p++)
            if (page_perm[i][(off - m->data_off) / PAGE_4K + p] & 16) {
                pthread_mutex_unlock(&perm_lock);
                return R_INVALID_CURRENT_MEMORY;
            }
        memset(page_perm[i] + (off - m->data_off) / PAGE_4K, (int)permission, size / PAGE_4K);
        /* A 16 KiB host page gets the union of its four guest pages' permissions, so protecting an edge never removes
         * access from a neighbour; interior pages really become read-only. */
        for (uint64_t page = off & ~(PAGE_16K - 1); page < page_up(off + size); page += PAGE_16K) {
            unsigned combined = 0;
            for (unsigned p = 0; p < 4; p++) combined |= data_page_perm(i, page + p * PAGE_4K);
            int prot = (combined & 1 ? PROT_READ : 0) | (combined & 2 ? PROT_WRITE : 0);
            if (combined & 4) runtime_fail("module permission overlaps executable page");
            if (mprotect(m->base + page, PAGE_16K, prot)) runtime_fail("module data permission: %s", strerror(errno));
        }
        pthread_mutex_unlock(&perm_lock);
        return 0;
    }
    return R_INVALID_CURRENT_MEMORY;
}
uint64_t module_set_attribute(uint64_t address, uint64_t size, unsigned mask, unsigned attribute) {
    if (address & 4095) return R_INVALID_ADDRESS;
    if (!size || (size & 4095)) return R_INVALID_SIZE;
    if (mask != 16 || attribute != 16) return R_INVALID_MEMORY_PERMISSIONS; /* the SDK only uses the irreversible RELRO lock */
    for (unsigned i = 0; i < g_nmodules; i++) {
        if (g_modules[i].gone) continue;
        Module *m = &g_modules[i];
        if (!in_data(m, address, size)) continue;
        pthread_mutex_lock(&perm_lock);
        ensure_perm(i);
        size_t first = (address - (uintptr_t)m->base - m->data_off) / PAGE_4K;
        for (uint64_t p = 0; p < size / PAGE_4K; p++) page_perm[i][first + p] |= 16;
        pthread_mutex_unlock(&perm_lock);
        return 0;
    }
    return R_INVALID_CURRENT_MEMORY;
}
int module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission, unsigned *attribute) {
    for (unsigned i = 0; i < g_nmodules; i++) {
        if (g_modules[i].gone) continue;
        Module *m = &g_modules[i];
        uint64_t start = (uintptr_t)m->base;
        if (address < start || address - start >= m->key_off) continue;
        uint64_t off = (address - start) & ~4095ULL, lo = off, hi = off + 4096;
        pthread_mutex_lock(&perm_lock);
        unsigned perm = data_page_perm(i, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->key_off : m->data_off;
        while (lo > limit && data_page_perm(i, lo - 4096) == perm) lo -= 4096;
        while (hi < end && data_page_perm(i, hi) == perm) hi += 4096;
        pthread_mutex_unlock(&perm_lock);
        *base = start + lo;
        *size = hi - lo;
        *state = data ? 4 : 3;
        *permission = perm & 7;
        *attribute = perm & 16;
        return 1;
    }
    return 0;
}
