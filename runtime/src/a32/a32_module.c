#include "a32_module.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NTB_HEADER 152u
#define R_ARM_ABS32 2u
#define R_ARM_GLOB_DAT 21u
#define R_ARM_JUMP_SLOT 22u
#define R_ARM_RELATIVE 23u
#define WINDOW_SIZE (1ull << 32)
#define MODULE_START 0x00200000u /* Horizon 32-bit ASLR region starts around here */

static void *read_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    void *buf = malloc((size_t)st.st_size);
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
    *size = got;
    return buf;
}
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
static unsigned fnv(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static const char *symname(const A32Module *m, unsigned i) { return m->strtab + m->syms[i].name; }

int a32_window_init(A32Window *w) {
    memset(w, 0, sizeof *w);
    /* A hint above 4 GiB keeps every guest address (< 2^32) valid as host + guest without colliding with the low 4 GiB the host binary uses. */
    void *p = mmap((void *)(uintptr_t)(8ull << 32), WINDOW_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    w->host = p;
    w->next_base = MODULE_START;
    return 0;
}
void a32_window_free(A32Window *w) {
    for (unsigned i = 0; i < w->nmods; i++) {
        free(w->mods[i].syms);
        free(w->mods[i].strtab);
        free(w->mods[i].rels);
        free(w->mods[i].hash);
    }
    for (unsigned i = 0; i < w->nhle; i++) free(w->hle_names[i]);
    free(w->hle_names);
    if (w->host) munmap(w->host, WINDOW_SIZE);
    memset(w, 0, sizeof *w);
}

static void build_hash(A32Module *m) {
    unsigned cap = 16;
    while (cap < m->nsym * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof(uint32_t));
    m->hash_mask = cap - 1;
    for (unsigned i = 1; m->hash && i < m->nsym; i++) {
        if (!m->syms[i].defined || m->syms[i].bind == 0) continue; /* exported = defined and not local */
        const char *s = symname(m, i);
        unsigned p = fnv(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, symname(m, m->hash[p]))) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}
static uint32_t module_symbol(const A32Module *m, const char *name) {
    if (!m->hash) return 0;
    unsigned p = fnv(name) & m->hash_mask;
    while (m->hash[p]) {
        const A32Sym *s = &m->syms[m->hash[p]];
        if (!strcmp(name, m->strtab + s->name)) return m->base + (uint32_t)s->value;
        p = (p + 1) & m->hash_mask;
    }
    return 0;
}
uint32_t a32_find_symbol(const A32Window *w, const char *name) {
    for (unsigned i = 0; i < w->nmods; i++) {
        uint32_t a = module_symbol(&w->mods[i], name);
        if (a) return a;
    }
    return 0;
}

int a32_module_map(A32Window *w, const char *dir, const char *name, char *err, size_t errn) {
    if (w->nmods == A32_MAX_MODULES) {
        snprintf(err, errn, "too many modules");
        return -1;
    }
    char path[1024];
    size_t isz = 0, tsz = 0;
    snprintf(path, sizeof path, "%s/%s.patched.bin", dir, name);
    uint8_t *img = read_file(path, &isz);
    snprintf(path, sizeof path, "%s/%s.tables", dir, name);
    uint8_t *tb = read_file(path, &tsz);
    int rc = -1;
    A32Module *m = &w->mods[w->nmods];
    memset(m, 0, sizeof *m);
    if (!img || !tb) {
        snprintf(err, errn, "%s: cannot read image or tables", name);
        goto out;
    }
    if (tsz < NTB_HEADER || memcmp(tb, "NTB2", 4)) {
        snprintf(err, errn, "%s: not an NTB2 table", name);
        goto out;
    }
    uint32_t nsym = rd32(tb + 8), nrel = rd32(tb + 12), strl = rd32(tb + 20);
    if ((uint64_t)NTB_HEADER + 24ull * nsym + 24ull * nrel + strl > tsz) {
        snprintf(err, errn, "%s: truncated tables", name);
        goto out;
    }
    uint64_t image_len = rd64(tb + 24);
    if (image_len != isz) {
        snprintf(err, errn, "%s: image size %zu, tables say %llu", name, isz, (unsigned long long)image_len);
        goto out;
    }
    uint32_t base = (w->next_base + 0xFFFu) & ~0xFFFu;
    if ((uint64_t)base + isz >= A32_HLE_BASE) {
        snprintf(err, errn, "%s: out of guest address space", name);
        goto out;
    }
    if (mprotect(w->host + base, isz, PROT_READ | PROT_WRITE)) {
        snprintf(err, errn, "%s: mprotect: %s", name, strerror(errno));
        goto out;
    }
    memcpy(w->host + base, img, isz);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base;
    m->size = (uint32_t)isz;
    m->text_end = (uint32_t)rd64(tb + 32);
    m->ro_off = (uint32_t)rd64(tb + 40);
    m->data_off = (uint32_t)rd64(tb + 48);
    m->dt_init = (uint32_t)rd64(tb + 72);
    m->dt_fini = (uint32_t)rd64(tb + 80);
    m->init_array = (uint32_t)rd64(tb + 88);
    m->init_array_sz = (uint32_t)rd64(tb + 96);
    m->fini_array = (uint32_t)rd64(tb + 104);
    m->fini_array_sz = (uint32_t)rd64(tb + 112);
    memcpy(m->build_id, tb + 120, 32);
    m->nsym = nsym;
    m->nrel = nrel;
    m->strtab_len = strl;
    m->syms = malloc(sizeof(A32Sym) * (nsym ? nsym : 1));
    m->rels = malloc(sizeof(A32Rel) * (nrel ? nrel : 1));
    m->strtab = malloc(strl + 1);
    if (!m->syms || !m->rels || !m->strtab) {
        snprintf(err, errn, "%s: out of memory", name);
        goto out;
    }
    const uint8_t *p = tb + NTB_HEADER;
    for (uint32_t i = 0; i < nsym; i++, p += 24) {
        A32Sym *s = &m->syms[i];
        s->name = rd32(p);
        s->bind = p[4];
        s->type = p[5];
        s->defined = p[6];
        s->value = rd64(p + 8);
        s->size = rd64(p + 16);
    }
    for (uint32_t i = 0; i < nrel; i++, p += 24) {
        A32Rel *r = &m->rels[i];
        r->where = rd64(p);
        r->addend = (int64_t)rd64(p + 8);
        r->kind = rd32(p + 16);
        r->sym = rd32(p + 20);
    }
    memcpy(m->strtab, p, strl);
    m->strtab[strl] = 0;
    for (uint32_t i = 0; i < nsym; i++)
        if (m->syms[i].name >= strl) {
            snprintf(err, errn, "%s: symbol name outside the string table", name);
            goto out;
        }
    build_hash(m);
    w->next_base = base + (uint32_t)((isz + 0xFFFFu) & ~0xFFFFu);
    w->nmods++;
    rc = 0;
out:
    if (rc) {
        free(m->syms);
        free(m->rels);
        free(m->strtab);
        free(m->hash);
        memset(m, 0, sizeof *m);
    }
    free(img);
    free(tb);
    return rc;
}

uint32_t a32_hle_stub(A32Window *w, const char *name) {
    for (unsigned i = 0; i < w->nhle; i++)
        if (!strcmp(w->hle_names[i], name)) return A32_HLE_BASE + 4 * i;
    if (w->nhle == w->caphle) {
        w->caphle = w->caphle ? w->caphle * 2 : 256;
        w->hle_names = realloc(w->hle_names, w->caphle * sizeof *w->hle_names);
    }
    w->hle_names[w->nhle] = strdup(name);
    return A32_HLE_BASE + 4 * w->nhle++;
}
const char *a32_hle_name(const A32Window *w, uint32_t a) {
    if (a < A32_HLE_BASE || (a & 3)) return NULL;
    uint32_t i = (a - A32_HLE_BASE) / 4;
    return i < w->nhle ? w->hle_names[i] : NULL;
}

int a32_relocate(A32Window *w, A32RelStats *st, char *err, size_t errn) {
    A32RelStats z = {0};
    if (!st) st = &z;
    *st = z;
    for (unsigned mi = 0; mi < w->nmods; mi++) {
        A32Module *m = &w->mods[mi];
        for (uint32_t i = 0; i < m->nrel; i++) {
            const A32Rel *r = &m->rels[i];
            if (r->where + 4 > m->size) {
                snprintf(err, errn, "%s: relocation %u outside the image", m->name, i);
                return -1;
            }
            uint32_t *slot = (uint32_t *)(w->host + m->base + r->where);
            uint32_t s = 0;
            if (r->kind != R_ARM_RELATIVE) {
                if (r->sym >= m->nsym) {
                    snprintf(err, errn, "%s: relocation %u: bad symbol", m->name, i);
                    return -1;
                }
                const A32Sym *sy = &m->syms[r->sym];
                if (w->is_override && w->is_override(symname(m, r->sym))) {
                    s = a32_hle_stub(w, symname(m, r->sym));
                    st->to_hle++;
                } else if (sy->defined) s = m->base + (uint32_t)sy->value;
                else {
                    s = a32_find_symbol(w, symname(m, r->sym));
                    if (s) st->to_module++;
                    else if (sy->bind == 2) {
                        w->weak_unresolved++;
                        s = 0;
                    } /* weak undefined resolves to 0 */
                    else {
                        s = a32_hle_stub(w, symname(m, r->sym));
                        st->to_hle++;
                    }
                }
            }
            switch (r->kind) {
                case R_ARM_RELATIVE:
                    *slot = m->base + (uint32_t)r->addend;
                    st->relative++;
                    break;
                case R_ARM_ABS32:
                    *slot = s + (uint32_t)r->addend;
                    st->abs32++;
                    break;
                case R_ARM_GLOB_DAT:
                    *slot = s;
                    st->glob_dat++;
                    break;
                case R_ARM_JUMP_SLOT:
                    *slot = s;
                    st->jump_slot++;
                    break;
                default: snprintf(err, errn, "%s: relocation kind %u", m->name, r->kind); return -1;
            }
            w->applied++;
        }
    }
    w->unresolved = w->nhle;
    return 0;
}
