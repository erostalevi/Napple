/* tinyjson.h - a small, strict JSON reader for configuration files (settings.json, input.json).

   No allocation: the document is parsed into a caller-provided node pool; strings point into the input text
   (escapes are decoded on demand by tj_str).  Limits: depth 16, pool size chosen by the caller.
   Identical copies live in input/src and frontend/src (the libraries are independent static libs);
   `make test` in both directories checks they are byte-identical. */
#ifndef NAPPLE_TINYJSON_H
#define NAPPLE_TINYJSON_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TJ_NULL, TJ_BOOL, TJ_NUM, TJ_STR, TJ_ARR, TJ_OBJ };
typedef struct tj_node {
    uint8_t type;
    uint8_t b;
    double num;
    const char *s;
    size_t n; /* string body (raw, escapes undecoded) or object key (in `key`) */
    const char *key;
    size_t key_n;
    int32_t child, next; /* indices into the pool, -1 = none */
} tj_node;
typedef struct {
    const char *p, *end;
    tj_node *pool;
    size_t cap, used;
    int depth;
} tj_parser;

static inline void tj_ws(tj_parser *t) {
    while (t->p < t->end && (*t->p == ' ' || *t->p == '\t' || *t->p == '\n' || *t->p == '\r')) t->p++;
}
static inline int tj_new(tj_parser *t) {
    if (t->used >= t->cap) return -1;
    tj_node *n = &t->pool[t->used];
    memset(n, 0, sizeof *n);
    n->child = n->next = -1;
    return (int)t->used++;
}
static inline int tj_string_span(tj_parser *t, const char **s, size_t *n) {
    if (t->p >= t->end || *t->p != '"') return -1;
    const char *b = ++t->p;
    while (t->p < t->end && *t->p != '"') {
        if (*t->p == '\\') t->p++;
        if (t->p < t->end && (unsigned char)*t->p < 0x20) return -1;
        t->p++;
    }
    if (t->p >= t->end) return -1;
    *s = b;
    *n = (size_t)(t->p - b);
    t->p++;
    return 0;
}
static inline int tj_value(tj_parser *t);
static inline int tj_container(tj_parser *t, int is_obj) {
    if (++t->depth > 16) return -1;
    int me = tj_new(t);
    if (me < 0) return -1;
    t->pool[me].type = is_obj ? TJ_OBJ : TJ_ARR;
    t->p++;
    int last = -1;
    tj_ws(t);
    if (t->p < t->end && *t->p == (is_obj ? '}' : ']')) {
        t->p++;
        t->depth--;
        return me;
    }
    for (;;) {
        tj_ws(t);
        const char *k = NULL;
        size_t kn = 0;
        if (is_obj) {
            if (tj_string_span(t, &k, &kn)) return -1;
            tj_ws(t);
            if (t->p >= t->end || *t->p++ != ':') return -1;
            tj_ws(t);
        }
        int c = tj_value(t);
        if (c < 0) return -1;
        t->pool[c].key = k;
        t->pool[c].key_n = kn;
        if (last < 0) t->pool[me].child = c;
        else t->pool[last].next = c;
        last = c;
        tj_ws(t);
        if (t->p >= t->end) return -1;
        if (*t->p == ',') {
            t->p++;
            continue;
        }
        if (*t->p == (is_obj ? '}' : ']')) {
            t->p++;
            break;
        }
        return -1;
    }
    t->depth--;
    return me;
}
static inline int tj_value(tj_parser *t) {
    tj_ws(t);
    if (t->p >= t->end) return -1;
    char c = *t->p;
    if (c == '{' || c == '[') return tj_container(t, c == '{');
    int id = tj_new(t);
    if (id < 0) return -1;
    tj_node *n = &t->pool[id];
    if (c == '"') {
        n->type = TJ_STR;
        return tj_string_span(t, &n->s, &n->n) ? -1 : id;
    }
    if ((size_t)(t->end - t->p) >= 4 && !strncmp(t->p, "true", 4)) {
        n->type = TJ_BOOL;
        n->b = 1;
        t->p += 4;
        return id;
    }
    if ((size_t)(t->end - t->p) >= 5 && !strncmp(t->p, "false", 5)) {
        n->type = TJ_BOOL;
        t->p += 5;
        return id;
    }
    if ((size_t)(t->end - t->p) >= 4 && !strncmp(t->p, "null", 4)) {
        n->type = TJ_NULL;
        t->p += 4;
        return id;
    }
    char buf[40];
    size_t k = 0;
    while (t->p < t->end && k < sizeof buf - 1 && strchr("+-0123456789.eE", *t->p)) buf[k++] = *t->p++;
    if (!k) return -1;
    buf[k] = 0;
    char *e;
    double v = strtod(buf, &e);
    if (*e || !isfinite(v)) return -1;
    n->type = TJ_NUM;
    n->num = v;
    return id;
}
/* Parses a whole document; returns the root node index (0) or -1. Trailing non-space data is an error. */
static inline int tj_parse(const char *text, size_t len, tj_node *pool, size_t cap) {
    tj_parser t = {text, text + len, pool, cap, 0, 0};
    int root = tj_value(&t);
    if (root < 0) return -1;
    tj_ws(&t);
    return t.p == t.end ? root : -1;
}
static inline const tj_node *tj_get(const tj_node *pool, const tj_node *obj, const char *key) {
    if (!obj || obj->type != TJ_OBJ) return NULL;
    size_t kl = strlen(key);
    for (int c = obj->child; c >= 0; c = pool[c].next)
        if (pool[c].key_n == kl && !memcmp(pool[c].key, key, kl)) return &pool[c];
    return NULL;
}
static inline const tj_node *tj_at(const tj_node *pool, const tj_node *arr, size_t i) {
    if (!arr || arr->type != TJ_ARR) return NULL;
    int c = arr->child;
    while (c >= 0 && i--) c = pool[c].next;
    return c >= 0 ? &pool[c] : NULL;
}
static inline size_t tj_len(const tj_node *pool, const tj_node *arr) {
    size_t n = 0;
    if (!arr || (arr->type != TJ_ARR && arr->type != TJ_OBJ)) return 0;
    for (int c = arr->child; c >= 0; c = pool[c].next) n++;
    return n;
}
static inline double tj_num(const tj_node *n, double fallback) { return n && n->type == TJ_NUM ? n->num : fallback; }
static inline int tj_bool(const tj_node *n, int fallback) { return n && n->type == TJ_BOOL ? n->b : n && n->type == TJ_NUM ? n->num != 0 : fallback; }
/* Decodes a string node into out (NUL terminated, truncated at cap-1). Handles \" \\ \/ \b \f \n \r \t \uXXXX (BMP). */
static inline size_t tj_str(const tj_node *n, char *out, size_t cap) {
    if (!cap) return 0;
    size_t o = 0;
    if (!n || n->type != TJ_STR) {
        out[0] = 0;
        return 0;
    }
    for (size_t i = 0; i < n->n && o + 4 < cap; i++) {
        char c = n->s[i];
        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        if (++i >= n->n) break;
        switch (n->s[i]) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int k = 0; k < 4 && i + 1 < n->n; k++) {
                    char h = n->s[++i];
                    v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                if (v < 0x80) out[o++] = (char)v;
                else if (v < 0x800) {
                    out[o++] = (char)(0xc0 | v >> 6);
                    out[o++] = (char)(0x80 | (v & 0x3f));
                } else {
                    out[o++] = (char)(0xe0 | v >> 12);
                    out[o++] = (char)(0x80 | ((v >> 6) & 0x3f));
                    out[o++] = (char)(0x80 | (v & 0x3f));
                }
                break;
            }
            default: out[o++] = n->s[i];
        }
    }
    out[o] = 0;
    return o;
}
/* Appends a JSON string literal (with escapes) to out[*at..cap); *at advances by the full length (snprintf style). */
static inline void tj_put_str(char *out, size_t cap, size_t *at, const char *s) {
#define TJ_PUT(c)                                                                                                                                              \
    do {                                                                                                                                                       \
        if (*at + 1 < cap) out[*at] = (c);                                                                                                                     \
        (*at)++;                                                                                                                                               \
    } while (0)
    TJ_PUT('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            TJ_PUT('\\');
            TJ_PUT((char)c);
        } else if (c == '\n') {
            TJ_PUT('\\');
            TJ_PUT('n');
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            for (char *q = b; *q; q++) TJ_PUT(*q);
        } else TJ_PUT((char)c);
    }
    TJ_PUT('"');
    if (cap) out[*at < cap ? *at : cap - 1] = 0;
#undef TJ_PUT
}
#endif
