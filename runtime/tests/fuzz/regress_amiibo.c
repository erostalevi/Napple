/* Regression tests for the crashes the amiibo fuzz target found (FINDINGS.md). Build with the sanitizers: make -C runtime fuzz-regress. */
#include "../../src/services/amiibo_core.h"
#include "../../src/services/amiibo_lib_core.h"
#include "../check.h"
#include <unistd.h>

int main(void) {
    alarm(20); /* a hang is a failure */

    /* 1. A stray closing brace inside the "amiibo" array: the array's extent counts `{` of `:{"b c"}` but the scalar scan of ami_jskip stops at the `}` after
     * `c"`, and ami_jskip returned the same position (an empty "number") there, so the array loop in amiibo_lib_load_text spun on it forever.
     * Found by fuzz amiibo, seed 1, iteration 501 (a hang; 497-byte input, a spliced AmiiboAPI document). */
    static const char stray[] = "{\"amiibo\":[{\"a\":1},:{\"b c\"}}]}";
    static const char stray2[] = "{\"amiibo\":[{\"head\":\"01010000\",\"tail\":\"00040002\",\"name\":\"A\"} ] ]}";
    AmiiboLib l;
    amiibo_lib_init(&l);
    CHECK(amiibo_lib_load_text(&l, stray, sizeof stray - 1) == -1, "a stray '}' in the array is a malformed document");
    amiibo_lib_load_text(&l, stray2, sizeof stray2 - 1);
    amiibo_lib_free(&l);
    AmiJVal v;
    CHECK(ami_jskip("}", (const char *)"}" + 1, &v) == NULL, "an empty value is refused by ami_jskip");
    CHECK(ami_jskip("12 ", (const char *)"12 " + 3, &v) != NULL && v.n == 2 && v.kind == 'n', "numbers still skip");

    /* 2. A key file whose magic-size byte is above 16: keygen copied `16 - magic_size` (negative) bytes into a 0x80 byte stack buffer (ASan:
     * negative-size-param in prepare_seed). A key file is user-supplied. Found by fuzz amiibo (the seed corpus' first test key). */
    uint8_t keyfile[160];
    memset(keyfile, 0x11, sizeof keyfile);
    keyfile[31] = 0xff;
    const char *path = "build/fuzz/regress_amiibo.key";
    FILE *f = fopen(path, "wb");
    if (!f) f = fopen("regress_amiibo.key", "wb"), path = "regress_amiibo.key";
    fwrite(keyfile, 1, sizeof keyfile, f);
    fclose(f);
    AmiiboKeys k;
    CHECK(amiibo_keys_load(path, &k) == -2, "a key file with magic size 255 is refused");
    remove(path);
    memset(&k, 0x22, sizeof k); /* the same key built by hand, bypassing the loader */
    k.data[31] = k.tag[31] = 0xff;
    uint8_t tag[AMIIBO_TAG_SIZE] = {0}, plain[AMIIBO_TAG_SIZE];
    amiibo_tag_unpack(&k, tag, plain);
    amiibo_tag_pack(&k, plain, tag);
    printf("{\"test\":\"regress_amiibo\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
