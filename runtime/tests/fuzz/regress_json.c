/* Regression tests for the problems the json fuzz target found (FINDINGS.md). Build with the sanitizers: make -C runtime fuzz-regress. */
#include "../../src/json.h"
#include "../check.h"
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* No quarantine: freed memory held back by ASan would count as RSS in the check below. */
const char *__asan_default_options(void) { return "detect_leaks=0:quarantine_size_mb=0"; }

/* The text is (pointer, length), not a C string: put it at the very end of an exactly sized heap block so a read past `len` is a heap-buffer-overflow. */
static JsonNode *parse_exact(const char *s) {
    size_t n = strlen(s);
    char *b = malloc(n ? n : 1);
    memcpy(b, s, n);
    JsonNode *r = json_parse(b, n);
    free(b);
    return r;
}

int main(void) {
    /* 1. strncmp("true"/"false"/"null") and strtod read past the end of a text that is not NUL-terminated (ASan: heap-buffer-overflow in strncmp,
     * json.c parse_value). Found by fuzz json, seed 1, iteration 1. */
    const char *cut[] = {"t", "tru", "fals", "nul", "[tru", "{\"a\":nul", "[1,2,12", "-", "1e", "[1.5e+", "{\"k\":-"};
    for (unsigned i = 0; i < sizeof cut / sizeof *cut; i++) json_free(parse_exact(cut[i]));
    JsonNode *n = parse_exact("12345");
    CHECK(n && n->type == J_NUM && n->num == 12345, "a number that ends with the text still parses");
    json_free(n);
    n = parse_exact("[true,false,null,-1.5e2]");
    CHECK(n && n->type == J_ARR && n->child->next->next->next->num == -150, "literals and numbers parse as before");
    json_free(n);
    CHECK(parse_exact("tru") == NULL && parse_exact("nul") == NULL, "a truncated literal is malformed");

    /* 2. Every string was allocated with room for the rest of the text and kept that way: 20000 short strings in an 80 KB document held 80 MB
     * (quadratic in the text size). They are shrunk to their length now. */
    size_t cnt = 20000;
    char *t = malloc(cnt * 4 + 16), *p = t;
    *p++ = '[';
    for (size_t i = 0; i < cnt; i++) p += sprintf(p, "\"a\",");
    p[-1] = ']';
    struct rusage a, b;
    getrusage(RUSAGE_SELF, &a);
    n = json_parse(t, (size_t)(p - t));
    getrusage(RUSAGE_SELF, &b);
    CHECK(n != NULL, "the array parses");
#ifdef __APPLE__
    b.ru_maxrss /= 1024; a.ru_maxrss /= 1024; /* bytes on macOS, KB on Linux */
#endif
    CHECK(b.ru_maxrss - a.ru_maxrss < 16 * 1024, "parsing an 80 KB document of short strings raised the peak RSS by %ld KB", b.ru_maxrss - a.ru_maxrss);
    json_free(n);
    free(t);
    printf("{\"test\":\"regress_json\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
