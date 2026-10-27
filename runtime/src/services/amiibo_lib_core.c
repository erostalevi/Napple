#include "amiibo_lib_core.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void amiibo_config_default(AmiiboConfig *c, const char *support) {
    memset(c, 0, sizeof *c);
    const char *home = getenv("HOME");
    const char *e;
    snprintf(c->source, sizeof c->source, "%s", (e = getenv("NAPPLE_AMIIBO_DB")) && *e ? e : AMIIBO_DEFAULT_SOURCE);
    snprintf(c->cache, sizeof c->cache, "%s/system/amiibo-cache/amiibo.json", support);
    snprintf(c->store, sizeof c->store, "%s", (e = getenv("NAPPLE_AMIIBO_STORE")) && *e ? e : "");
    if (!c->store[0]) snprintf(c->store, sizeof c->store, "%s/system/amiibo", support);
    snprintf(c->key, sizeof c->key, "%s", (e = getenv("NAPPLE_AMIIBO_KEY")) && *e ? e : "");
    if (!c->key[0]) snprintf(c->key, sizeof c->key, "%s/.switch/%s", home ? home : ".", AMIIBO_KEY_FILE_NAME);
}
int amiibo_config_source_is_remote(const AmiiboConfig *c) { return !strncmp(c->source, "http://", 7) || !strncmp(c->source, "https://", 8); }

void amiibo_lib_init(AmiiboLib *l) { memset(l, 0, sizeof *l); }
void amiibo_lib_free(AmiiboLib *l) {
    free(l->items);
    memset(l, 0, sizeof *l);
}
static void cpy(char *dst, size_t cap, const AmiJVal *v) {
    char tmp[512];
    if (v && v->kind == 's' && ami_jstr(v, tmp, sizeof tmp) >= 0) snprintf(dst, cap, "%s", tmp);
}
static int cmp_name(const void *a, const void *b) {
    const AmiiboInfo *x = a, *y = b;
    int r = strcasecmp(x->name, y->name);
    return r ? r : strcmp(x->id, y->id);
}
static int push(AmiiboLib *l, const AmiiboInfo *it) {
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 256;
        AmiiboInfo *n = realloc(l->items, nc * sizeof *n);
        if (!n) return -1;
        l->items = n;
        l->cap = nc;
    }
    l->items[l->count++] = *it;
    return 0;
}
long amiibo_lib_load_text(AmiiboLib *l, const char *text, size_t n) {
    AmiiboLib t;
    amiibo_lib_init(&t);
    AmiJVal arr;
    const char *e = text + n;
    int rc = ami_jget(text, n, "amiibo", &arr);
    if (rc == 0 && arr.kind == 'a') {
        const char *p = arr.p + 1, *end = arr.p + arr.n - 1;
        for (;;) {
            p = ami_jws(p, end);
            if (p >= end) break;
            if (*p == ',') {
                p++;
                continue;
            }
            AmiJVal el;
            p = ami_jskip(p, end, &el);
            if (!p) {
                amiibo_lib_free(&t);
                return -1;
            }
            if (el.kind != 'o') continue;
            AmiJVal head, tail, v;
            AmiiboInfo it;
            memset(&it, 0, sizeof it);
            if (ami_jget(el.p, el.n, "head", &head) || ami_jget(el.p, el.n, "tail", &tail) || head.kind != 's' || tail.kind != 's' || head.n != 8 ||
                tail.n != 8)
                continue;
            memcpy(it.id, head.p, 8);
            memcpy(it.id + 8, tail.p, 8);
            uint8_t chk[8];
            if (amiibo_id_from_hex(it.id, chk)) continue;
            for (int i = 0; i < 16; i++) it.id[i] = (char)tolower((unsigned char)it.id[i]);
            if (ami_jget(el.p, el.n, "name", &v) == 0) cpy(it.name, sizeof it.name, &v);
            if (ami_jget(el.p, el.n, "character", &v) == 0) cpy(it.character, sizeof it.character, &v);
            if (ami_jget(el.p, el.n, "amiiboSeries", &v) == 0) cpy(it.series, sizeof it.series, &v);
            if (ami_jget(el.p, el.n, "gameSeries", &v) == 0) cpy(it.game_series, sizeof it.game_series, &v);
            if (ami_jget(el.p, el.n, "type", &v) == 0) cpy(it.type, sizeof it.type, &v);
            if (ami_jget(el.p, el.n, "image", &v) == 0) cpy(it.image, sizeof it.image, &v);
            if (push(&t, &it)) {
                amiibo_lib_free(&t);
                return -1;
            }
        }
    } else if (rc == 1 && ami_jget(text, n, "amiibos", &arr) == 0 && arr.kind == 'o') {
        /* the AmiiboAPI repository's full database: {"amiibos": {"0x<id>": {"name"}}, "characters", "amiibo_series", "game_series", "types"} */
        AmiJVal tbl[4];
        static const char *const tn[4] = {"characters", "amiibo_series", "game_series", "types"};
        for (int i = 0; i < 4; i++)
            if (ami_jget(text, n, tn[i], &tbl[i]) || tbl[i].kind != 'o') tbl[i].kind = 0;
        const char *cur = arr.p;
        AmiJVal k, v;
        int r;
        while ((r = ami_jiter(&cur, arr.p + arr.n, &k, &v)) == 1) {
            if (k.n != 18 || k.p[0] != '0' || (k.p[1] != 'x' && k.p[1] != 'X') || v.kind != 'o') continue;
            AmiiboInfo it;
            memset(&it, 0, sizeof it);
            uint8_t chk[8];
            memcpy(it.id, k.p + 2, 16);
            if (amiibo_id_from_hex(it.id, chk)) continue;
            for (int i = 0; i < 16; i++) it.id[i] = (char)tolower((unsigned char)it.id[i]);
            AmiJVal nm;
            if (ami_jget(v.p, v.n, "name", &nm) == 0) cpy(it.name, sizeof it.name, &nm);
            /* lookup keys: character = id[0..3], game series = id[0..2], type = id[6..7], amiibo series = id[12..13] */
            char key[8];
            AmiJVal x;
            snprintf(key, sizeof key, "0x%.4s", it.id);
            if (tbl[0].kind && ami_jget(tbl[0].p, tbl[0].n, key, &x) == 0) cpy(it.character, sizeof it.character, &x);
            snprintf(key, sizeof key, "0x%.2s", it.id + 12);
            if (tbl[1].kind && ami_jget(tbl[1].p, tbl[1].n, key, &x) == 0) cpy(it.series, sizeof it.series, &x);
            snprintf(key, sizeof key, "0x%.3s", it.id);
            if (tbl[2].kind && ami_jget(tbl[2].p, tbl[2].n, key, &x) == 0) cpy(it.game_series, sizeof it.game_series, &x);
            snprintf(key, sizeof key, "0x%.2s", it.id + 6);
            if (tbl[3].kind && ami_jget(tbl[3].p, tbl[3].n, key, &x) == 0) cpy(it.type, sizeof it.type, &x);
            snprintf(it.image, sizeof it.image, AMIIBO_REPO_IMAGE_FMT, it.id, it.id + 8);
            if (push(&t, &it)) {
                amiibo_lib_free(&t);
                return -1;
            }
        }
        if (r < 0) {
            amiibo_lib_free(&t);
            return -1;
        }
    } else if (rc == 1) {
        /* repository database: {"0x0000000000000002": {"name": "..."}, ...} */
        const char *cur = text;
        AmiJVal k, v;
        int r;
        while ((r = ami_jiter(&cur, e, &k, &v)) == 1) {
            if (k.n != 18 || k.p[0] != '0' || (k.p[1] != 'x' && k.p[1] != 'X') || v.kind != 'o') continue;
            AmiiboInfo it;
            memset(&it, 0, sizeof it);
            uint8_t chk[8];
            memcpy(it.id, k.p + 2, 16);
            if (amiibo_id_from_hex(it.id, chk)) continue;
            for (int i = 0; i < 16; i++) it.id[i] = (char)tolower((unsigned char)it.id[i]);
            AmiJVal nm;
            if (ami_jget(v.p, v.n, "name", &nm) == 0) cpy(it.name, sizeof it.name, &nm);
            if (push(&t, &it)) {
                amiibo_lib_free(&t);
                return -1;
            }
        }
        if (r < 0) {
            amiibo_lib_free(&t);
            return -1;
        }
    }
    if (!t.count) {
        amiibo_lib_free(&t);
        return -1;
    }
    qsort(t.items, t.count, sizeof *t.items, cmp_name);
    amiibo_lib_free(l);
    *l = t;
    return (long)t.count;
}
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (64l << 20)) {
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    b[sz] = 0;
    *n = (size_t)sz;
    return b;
}
long amiibo_lib_load_file(AmiiboLib *l, const char *path) {
    size_t n;
    char *b = slurp(path, &n);
    if (!b) return -1;
    long r = amiibo_lib_load_text(l, b, n);
    free(b);
    return r;
}
long amiibo_lib_load_source(AmiiboLib *l, const char *path) {
    struct stat st;
    if (stat(path, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return amiibo_lib_load_file(l, path);
    static const char *const rel[] = {"amiibo.json", "database/amiibo.json", "api/amiibo/index.json"};
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", path, rel[i]);
        long r = amiibo_lib_load_file(l, p);
        if (r > 0) return r;
    }
    return -1;
}
static void mkdirs_for(const char *file) {
    char b[600];
    snprintf(b, sizeof b, "%s", file);
    for (char *p = b + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0755);
            *p = '/';
        }
}
int amiibo_lib_refresh(AmiiboLib *l, const AmiiboConfig *c, const char *fetched, size_t n) {
    if (fetched && n && amiibo_lib_load_text(l, fetched, n) > 0) {
        mkdirs_for(c->cache);
        char tmp[620];
        snprintf(tmp, sizeof tmp, "%s.tmp", c->cache);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(fetched, 1, n, f) == n;
            ok &= fclose(f) == 0;
            if (!ok || rename(tmp, c->cache)) remove(tmp);
        }
        return 1;
    }
    if (amiibo_lib_load_file(l, c->cache) > 0) return 2;
    if (!amiibo_config_source_is_remote(c) && amiibo_lib_load_source(l, c->source) > 0) return 3;
    return -1;
}
const AmiiboInfo *amiibo_lib_find(const AmiiboLib *l, const uint8_t id[8]) {
    char h[17];
    amiibo_id_to_hex(id, h);
    for (size_t i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].id, h)) return &l->items[i];
    return NULL;
}
static int ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}
size_t amiibo_lib_search(const AmiiboLib *l, const char *q, size_t *idx, size_t max) {
    size_t total = 0;
    for (size_t i = 0; i < l->count; i++) {
        const AmiiboInfo *a = &l->items[i];
        if (ci_contains(a->name, q) || ci_contains(a->character, q) || ci_contains(a->series, q) || ci_contains(a->game_series, q) || ci_contains(a->id, q)) {
            if (idx && total < max) idx[total] = i;
            total++;
        }
    }
    return total;
}

/* ---- picker ----------------------------------------------------------------------------------------------------------------------- */
static int src_poll(void *ctx, NfcTag *out, uint64_t *gen) {
    AmiiboPicker *p = ctx;
    pthread_mutex_lock(&p->lock);
    int present = p->present;
    if (present) *out = p->tag;
    *gen = p->generation;
    pthread_mutex_unlock(&p->lock);
    return present;
}
static int src_store(void *ctx, const NfcTag *t) {
    AmiiboPicker *p = ctx;
    if (!t->is_amiibo) return -1;
    int r = amiibo_store_save(p->cfg.store, &t->amiibo);
    pthread_mutex_lock(&p->lock);
    if (p->present && !memcmp(p->tag.uid, t->uid, 7)) p->tag.amiibo = t->amiibo; /* the reader sees what was stored */
    pthread_mutex_unlock(&p->lock);
    return r;
}
static void src_listener(void *ctx, void (*cb)(void *), void *cbctx) {
    AmiiboPicker *p = ctx;
    pthread_mutex_lock(&p->lock);
    p->listener = cb;
    p->lctx = cbctx;
    pthread_mutex_unlock(&p->lock);
}
void amiibo_picker_init(AmiiboPicker *p, const AmiiboConfig *c) {
    memset(p, 0, sizeof *p);
    p->cfg = *c;
    amiibo_lib_init(&p->lib);
    pthread_mutex_init(&p->lock, NULL);
    p->source = (NfcTagSource){.ctx = p, .name = "amiibo picker", .poll = src_poll, .store = src_store, .set_listener = src_listener};
}
void amiibo_picker_free(AmiiboPicker *p) {
    amiibo_lib_free(&p->lib);
    pthread_mutex_destroy(&p->lock);
}
const NfcTagSource *amiibo_picker_source(AmiiboPicker *p) { return &p->source; }
size_t amiibo_picker_count(const AmiiboPicker *p) { return p->lib.count; }
const AmiiboInfo *amiibo_picker_get(const AmiiboPicker *p, size_t i) { return i < p->lib.count ? &p->lib.items[i] : NULL; }
size_t amiibo_picker_search(const AmiiboPicker *p, const char *q, size_t *idx, size_t max) { return amiibo_lib_search(&p->lib, q, idx, max); }

static void place(AmiiboPicker *p, const Amiibo *a) {
    pthread_mutex_lock(&p->lock);
    memset(&p->tag, 0, sizeof p->tag);
    memcpy(p->tag.uid, a->uid, 7);
    p->tag.is_amiibo = 1;
    p->tag.amiibo = *a;
    p->present = 1;
    p->generation++;
    void (*cb)(void *) = p->listener;
    void *cx = p->lctx;
    pthread_mutex_unlock(&p->lock);
    if (cb) cb(cx);
}
int amiibo_picker_select(AmiiboPicker *p, const uint8_t id[8], int flags) {
    Amiibo a;
    p->last_error[0] = 0;
    if (flags == AMIIBO_PICK_REUSE && p->cfg.store[0] && amiibo_store_find_by_id(p->cfg.store, id, &a) == 0) {
        place(p, &a);
        return 0;
    }
    amiibo_new(&a, id, 0);
    if (p->cfg.store[0] && amiibo_store_save(p->cfg.store, &a)) {
        snprintf(p->last_error, sizeof p->last_error, "cannot write the amiibo store at %s", p->cfg.store);
        return -2;
    }
    place(p, &a);
    return 0;
}
int amiibo_picker_select_hex(AmiiboPicker *p, const char *hex, int flags) {
    uint8_t id[8];
    if (amiibo_id_from_hex(hex, id)) {
        snprintf(p->last_error, sizeof p->last_error, "'%s' is not a 16-digit amiibo id", hex ? hex : "");
        return -1;
    }
    return amiibo_picker_select(p, id, flags);
}
int amiibo_picker_select_uid(AmiiboPicker *p, const uint8_t uid[7]) {
    Amiibo a;
    if (amiibo_store_load(p->cfg.store, uid, &a)) {
        snprintf(p->last_error, sizeof p->last_error, "no stored amiibo with that UID in %s", p->cfg.store);
        return -1;
    }
    place(p, &a);
    return 0;
}
int amiibo_picker_import_dump(AmiiboPicker *p, const char *dump_path) {
    uint8_t tag[AMIIBO_TAG_SIZE + 1];
    p->last_error[0] = 0;
    FILE *f = fopen(dump_path, "rb");
    size_t n = f ? fread(tag, 1, sizeof tag, f) : 0;
    if (f) fclose(f);
    if (n != AMIIBO_TAG_SIZE && n != 540 + 32) { /* some tools append a 32-byte signature block */
        snprintf(p->last_error, sizeof p->last_error, "%s is not a 540-byte NTAG215 dump", dump_path);
        return -1;
    }
    AmiiboKeys k;
    int kr = amiibo_keys_load(p->cfg.key, &k);
    if (kr == -1) {
        snprintf(p->last_error, sizeof p->last_error, "amiibo key file missing: %s (supply %s like prod.keys; it is never shipped)", p->cfg.key,
                 AMIIBO_KEY_FILE_NAME);
        return -2;
    }
    if (kr) {
        snprintf(p->last_error, sizeof p->last_error, "%s is not a valid %s (160 bytes expected)", p->cfg.key, AMIIBO_KEY_FILE_NAME);
        return -3;
    }
    Amiibo a;
    if (amiibo_import_dump(&k, tag, &a)) {
        snprintf(p->last_error, sizeof p->last_error, "%s did not verify with %s (wrong key or damaged dump)", dump_path, p->cfg.key);
        return -4;
    }
    if (p->cfg.store[0]) amiibo_store_save(p->cfg.store, &a);
    place(p, &a);
    return 0;
}
void amiibo_picker_remove(AmiiboPicker *p) {
    pthread_mutex_lock(&p->lock);
    int was = p->present;
    p->present = 0;
    if (was) p->generation++;
    void (*cb)(void *) = p->listener;
    void *cx = p->lctx;
    pthread_mutex_unlock(&p->lock);
    if (was && cb) cb(cx);
}
int amiibo_picker_present(AmiiboPicker *p) {
    pthread_mutex_lock(&p->lock);
    int v = p->present;
    pthread_mutex_unlock(&p->lock);
    return v;
}

int amiibo_script_parse(const char *spec, AmiiboScript *s) {
    memset(s, 0, sizeof *s);
    if (!spec || !*spec) return -1;
    const char *c1 = strchr(spec, ',');
    size_t wl = c1 ? (size_t)(c1 - spec) : strlen(spec);
    if (!wl || wl >= sizeof s->what) return -1;
    memcpy(s->what, spec, wl);
    s->what[wl] = 0;
    if (c1) {
        char *end;
        unsigned long a = strtoul(c1 + 1, &end, 10);
        s->insert_ms = (unsigned)a;
        if (*end == ',') s->remove_ms = (unsigned)strtoul(end + 1, NULL, 10);
        else if (*end) return -1;
    }
    s->valid = 1;
    return 0;
}
int amiibo_script_apply(AmiiboPicker *p, const AmiiboScript *s) {
    size_t l = strlen(s->what);
    uint8_t uid[7];
    if (l == 16 || (l == 18 && s->what[0] == '0' && (s->what[1] | 0x20) == 'x')) return amiibo_picker_select_hex(p, s->what, AMIIBO_PICK_REUSE);
    if (l == 14 && ami_hex_to_bytes(s->what, 14, uid) == 0) return amiibo_picker_select_uid(p, uid);
    return amiibo_picker_import_dump(p, s->what);
}
