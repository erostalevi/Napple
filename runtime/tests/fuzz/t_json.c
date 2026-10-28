/* Fuzz target: the minimal JSON reader (src/json.c) that reads the data directory's manifest (a file the user's build tool wrote, but anyone can hand
 * around data directories). The text is given as (pointer, length) with no terminator. */
#include "../../src/json.h"
#include "fuzz.h"
#include <stdlib.h>
#include <string.h>

static unsigned walk(const JsonNode *n, unsigned depth) {
    unsigned count = 0;
    for (; n; n = n->next) {
        count++;
        if (n->type == J_STR && n->str) count += (unsigned)strlen(n->str) & 1;
        if (n->key) count += (unsigned)strlen(n->key) & 1;
        if (depth < 64) count += walk(n->child, depth + 1);
        json_str(n, "name", "x"), json_num(n, "size", 0), json_get(n, "files");
    }
    return count;
}

int fuzz_json(const uint8_t *d, size_t n) {
    JsonNode *root = json_parse((const char *)d, n);
    if (root) walk(root, 0);
    json_free(root);
    return 0;
}

void fuzz_json_seeds(FuzzSeedFn add, void *ctx) {
    static const char *const docs[] = {
        "{\"format\":\"NTB1\",\"title\":\"Game\",\"program_id\":\"0100000000010000\",\"size\":123456,\"debug\":false,\"x\":null,\"files\":[{\"name\":\"main\",\"size\":1.5e3,"
        "\"ok\":true},{\"name\":\"rtld\\n\\t\\\"q\\\"\\u00e9\",\"size\":-7}]}",
        "[1,2,3,[4,[5,[6]]],{\"a\":{\"b\":{\"c\":[]}}}]",
        "{\"modules\":[{\"id\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\",\"file\":\"a.tables\",\"hooks\":[]}]}",
        "\"just a string\"",
        "-12.5e-3",
        "{\"a\":true,\"b\":false,\"c\":null,\"d\":\"x\",\"e\":[true,false,null]}",
    };
    for (unsigned i = 0; i < sizeof docs / sizeof *docs; i++) add(ctx, (const uint8_t *)docs[i], strlen(docs[i]));
}
FUZZ_TARGET(json);
