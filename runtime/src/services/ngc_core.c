#include "ngc_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t fold(uint16_t u) { return (u >= 'A' && u <= 'Z') ? (uint16_t)(u + 32) : u; }
void ngc_words_clear(NgcWords *w) {
    uint32_t v = w->version;
    memset(w, 0, sizeof *w);
    w->version = v + 1;
}
unsigned ngc_words_load_text(NgcWords *w, const char *text, size_t n) {
    ngc_words_clear(w);
    size_t i = 0;
    while (i < n && w->count < NGC_MAX_WORDS) {
        size_t e = i;
        while (e < n && text[e] != '\n' && text[e] != '\r') e++;
        if (e > i && text[i] != '#') {
            unsigned k = 0;
            for (size_t j = i; j < e && k < NGC_MAX_WORD_LEN; j++) w->w[w->count][k++] = fold((uint8_t)text[j]);
            if (k) w->len[w->count++] = (uint8_t)k;
        }
        i = e + 1;
    }
    return w->count;
}
unsigned ngc_words_load_env(NgcWords *w) {
    const char *path = getenv("NAPPLE_NGC_WORDLIST");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) {
        ngc_words_clear(w);
        return 0;
    }
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return ngc_words_load_text(w, buf, n);
}
static int match_at(const NgcWords *w, const uint16_t *t, size_t n, size_t pos) {
    int best = 0;
    for (unsigned i = 0; i < w->count; i++) {
        unsigned l = w->len[i];
        if (pos + l > n || (int)l <= best) continue;
        unsigned k = 0;
        while (k < l && fold(t[pos + k]) == w->w[i][k]) k++;
        if (k == l) best = (int)l;
    }
    return best;
}
unsigned ngc_check(const NgcWords *w, const uint16_t *text, size_t n) {
    unsigned hits = 0;
    for (size_t i = 0; i < n;) {
        int l = match_at(w, text, n, i);
        if (l) hits++, i += (size_t)l;
        else i++;
    }
    return hits;
}
size_t ngc_mask(const NgcWords *w, uint16_t *text, size_t n) {
    size_t masked = 0;
    for (size_t i = 0; i < n;) {
        int l = match_at(w, text, n, i);
        if (l) {
            for (int k = 0; k < l; k++) text[i + (size_t)k] = '*';
            masked += (size_t)l;
            i += (size_t)l;
        } else i++;
    }
    return masked;
}

void ngc_words_commit(NgcWords *w) { w->version++; }
static int pattern_meta(uint16_t u) { return u == '[' || u == ']' || u == '(' || u == ')' || u == '{' || u == '}' || u == '+' || u == '?' || u == '|' || u == '^' || u == '*'; }
unsigned ngc_words_add_ngword(NgcWords *w, const uint8_t *d, size_t n) {
    unsigned added = 0;
    size_t i = 0;
    if (n >= 2 && d[0] == 0xfe && d[1] == 0xff) i = 2;
    while (i + 1 < n && w->count < NGC_MAX_WORDS) {
        uint16_t line[2 * NGC_MAX_WORD_LEN + 8];
        size_t k = 0;
        int overflow = 0;
        for (; i + 1 < n; i += 2) {
            uint16_t u = (uint16_t)(d[i] << 8 | d[i + 1]);
            if (u == '\n') {
                i += 2;
                break;
            }
            if (k < sizeof line / sizeof *line) line[k++] = u;
            else overflow = 1;
        }
        while (k && line[k - 1] == '\r') k--;
        if (overflow || k < 5 || line[0] != '.' || line[1] != '*' || line[k - 2] != '.' || line[k - 1] != '*') continue;
        uint16_t word[NGC_MAX_WORD_LEN];
        unsigned wl = 0;
        int ok = 1;
        for (size_t j = 2; j < k - 2 && ok; j++) {
            uint16_t u = line[j];
            if (u == '\\') {
                if (++j >= k - 2) ok = 0;
                else u = line[j];
            } else if (pattern_meta(u) || u == '.') ok = 0;
            if (ok && wl < NGC_MAX_WORD_LEN) word[wl++] = fold(u);
            else if (ok) ok = 0;
        }
        if (!ok || !wl) continue;
        int dup = 0;
        for (unsigned a = 0; a < w->count && !dup; a++) dup = w->len[a] == wl && !memcmp(w->w[a], word, wl * sizeof *word);
        if (dup) continue;
        memcpy(w->w[w->count], word, wl * sizeof *word);
        w->len[w->count++] = (uint8_t)wl;
        added++;
    }
    return added;
}
