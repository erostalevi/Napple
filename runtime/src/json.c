#include "json.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p, *end;
    int depth;
} Cur;

static void ws(Cur *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\n' || *c->p == '\t' || *c->p == '\r')) c->p++;
}
static JsonNode *node(int type) {
    JsonNode *n = calloc(1, sizeof *n);
    if (n) n->type = type;
    return n;
}

static char *parse_string(Cur *c) {
    if (c->p >= c->end || *c->p != '"') return NULL;
    c->p++;
    char *out = malloc((size_t)(c->end - c->p) + 1), *o = out;
    if (!out) return NULL;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\' && c->p + 1 < c->end) {
            c->p++;
            switch (*c->p) {
                case 'n': *o++ = '\n'; break;
                case 't': *o++ = '\t'; break;
                case 'r': *o++ = '\r'; break;
                case 'u':
                    if (c->end - c->p >= 5) {
                        c->p += 4;
                        *o++ = '?';
                    }
                    break;
                default: *o++ = *c->p;
            }
            c->p++;
        } else *o++ = *c->p++;
    }
    if (c->p >= c->end) {
        free(out);
        return NULL;
    }
    c->p++;
    *o = 0;
    return out;
}

static JsonNode *parse_value(Cur *c) {
    ws(c);
    if (c->p >= c->end || ++c->depth > 32) return NULL;
    JsonNode *n = NULL;
    char ch = *c->p;
    if (ch == '"') {
        n = node(J_STR);
        if (n && !(n->str = parse_string(c))) {
            free(n);
            n = NULL;
        }
    } else if (ch == '{' || ch == '[') {
        int obj = ch == '{';
        n = node(obj ? J_OBJ : J_ARR);
        c->p++;
        JsonNode **tail = n ? &n->child : NULL;
        for (;;) {
            ws(c);
            if (c->p >= c->end) {
                json_free(n);
                n = NULL;
                break;
            }
            if (*c->p == (obj ? '}' : ']')) {
                c->p++;
                break;
            }
            char *key = NULL;
            if (obj) {
                key = parse_string(c);
                ws(c);
                if (!key || c->p >= c->end || *c->p++ != ':') {
                    free(key);
                    json_free(n);
                    n = NULL;
                    break;
                }
            }
            JsonNode *v = parse_value(c);
            if (!v) {
                free(key);
                json_free(n);
                n = NULL;
                break;
            }
            v->key = key;
            *tail = v;
            tail = &v->next;
            ws(c);
            if (c->p < c->end && *c->p == ',') c->p++;
        }
    } else if (!strncmp(c->p, "true", 4) || !strncmp(c->p, "false", 5)) {
        n = node(J_BOOL);
        n->num = *c->p == 't';
        c->p += n->num ? 4 : 5;
    } else if (!strncmp(c->p, "null", 4)) {
        n = node(J_NULL);
        c->p += 4;
    } else {
        char *e;
        n = node(J_NUM);
        n->num = strtod(c->p, &e);
        if (e == c->p) {
            free(n);
            n = NULL;
        } else c->p = e;
    }
    c->depth--;
    return n;
}

JsonNode *json_parse(const char *text, size_t len) {
    Cur c = {text, text + len, 0};
    return parse_value(&c);
}
const JsonNode *json_get(const JsonNode *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (const JsonNode *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}
const char *json_str(const JsonNode *obj, const char *key, const char *fallback) {
    const JsonNode *n = json_get(obj, key);
    return n && n->type == J_STR ? n->str : fallback;
}
double json_num(const JsonNode *obj, const char *key, double fallback) {
    const JsonNode *n = json_get(obj, key);
    return n && n->type == J_NUM ? n->num : fallback;
}
void json_free(JsonNode *n) {
    while (n) {
        JsonNode *next = n->next;
        json_free(n->child);
        free(n->str);
        free(n->key);
        free(n);
        n = next;
    }
}
