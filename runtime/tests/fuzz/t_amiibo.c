/* Fuzz target: the amiibo loaders (services/amiibo_core.c, amiibo_lib_core.c): the virtual amiibo JSON file, the AmiiboAPI library documents (both forms), the
 * small JSON reader they share, base64/hex, the NTAG215 dump import and the key file. All of it reads user-supplied files and downloaded documents.
 * Input: [mode][bytes]; the mode picks the entry point (see the switch). */
#include "../../src/services/amiibo_core.h"
#include "../../src/services/amiibo_lib_core.h"
#include "fuzz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void test_keys(AmiiboKeys *k) { /* not a real key: derived from a counter (the unit tests do the same) */
    for (int i = 0; i < 80; i++) k->data[i] = (uint8_t)(i * 3 + 1), k->tag[i] = (uint8_t)(i * 5 + 2);
    memcpy(k->data + 16, "unfixed infos\0", 14), memcpy(k->tag + 16, "locked secret\0", 14); /* type strings */
    k->data[30] = k->tag[30] = 0, k->data[31] = 14, k->tag[31] = 16;                         /* rfu, magic size */
}

int fuzz_amiibo(const uint8_t *d, size_t n) {
    if (!n) return 0;
    unsigned mode = d[0] % 8;
    d++, n--;
    char *z = malloc(n + 1); /* NUL-terminated copy for the C-string entry points */
    memcpy(z, d, n);
    z[n] = 0;
    char text[1024];
    switch (mode) {
        case 0: { /* the amiibo file: parse, write back, parse that again (a document the writer produced always reads back) */
            Amiibo a;
            memset(&a, 0, sizeof a);
            if (amiibo_from_json((const char *)d, n, &a) == 0) {
                static char out[16384];
                size_t m = amiibo_to_json(&a, out, sizeof out);
                Amiibo b;
                if (m && amiibo_from_json(out, m, &b) != 0) __builtin_trap();
            }
            break;
        }
        case 1: { /* the library documents */
            AmiiboLib l;
            amiibo_lib_init(&l);
            long cnt = amiibo_lib_load_text(&l, (const char *)d, n);
            if (cnt > 0) {
                size_t idx[8];
                amiibo_lib_search(&l, "mario", idx, 8);
                amiibo_lib_search(&l, z, idx, 8);
                amiibo_lib_search(&l, "", idx, 8);
                uint8_t id[8];
                if (n >= 8) memcpy(id, d, 8);
                else memset(id, 1, 8);
                amiibo_lib_find(&l, id);
                if (l.count) {
                    uint8_t id2[8];
                    if (amiibo_id_from_hex(l.items[0].id, id2) == 0) amiibo_lib_find(&l, id2);
                }
            }
            amiibo_lib_free(&l);
            break;
        }
        case 2: { /* the shared JSON reader: members of the top-level object, nested values, string decoding */
            AmiJVal v, k, val;
            for (const char *key = z; *key && key < z + 4; key++) {
                char kk[2] = {*key, 0};
                if (ami_jget((const char *)d, n, kk, &v) == 0) ami_jstr(&v, text, sizeof text);
            }
            const char *cur = (const char *)d;
            int guard = 0;
            while (ami_jiter(&cur, (const char *)d + n, &k, &val) == 1 && guard++ < 4096) {
                ami_jstr(&k, text, sizeof text);
                ami_jstr(&val, text, 8);
                if (val.kind == 'o' || val.kind == 'a') ami_jget(val.p, val.n, "name", &v);
            }
            ami_jskip(ami_jws((const char *)d, (const char *)d + n), (const char *)d + n, &v);
            break;
        }
        case 3: { /* base64 and hex */
            uint8_t out[512];
            long m = ami_b64_decode((const char *)d, n, out, sizeof out);
            if (m >= 0) {
                char enc[1024];
                size_t e = ami_b64_encode(out, (size_t)m, enc, sizeof enc);
                if ((long)e < 0) __builtin_trap();
            }
            ami_b64_decode((const char *)d, n, out, 7);
            ami_hex_to_bytes(z, n > 64 ? 64 : n & ~(size_t)1, out);
            uint8_t id[8];
            amiibo_id_from_hex(z, id);
            break;
        }
        case 4:
        case 5: { /* a dump: unpack with a test key (the signature check rejects nearly everything), and the plain layout directly */
            uint8_t tag[AMIIBO_TAG_SIZE], plain[AMIIBO_TAG_SIZE];
            memset(tag, 0, sizeof tag);
            memcpy(tag, d, n < sizeof tag ? n : sizeof tag);
            Amiibo a;
            AmiiboKeys k;
            test_keys(&k);
            if (mode == 4) {
                amiibo_tag_unpack(&k, tag, plain);
                amiibo_import_dump(&k, tag, &a);
            } else if (amiibo_from_plain(tag, &a) == 0) {
                amiibo_to_plain(&a, plain);
                amiibo_export_dump(&k, &a, tag);
                amiibo_import_dump(&k, tag, &a);
            }
            break;
        }
        case 6: { /* the key file and an amiibo store file, through real files */
            char path[600];
            snprintf(path, sizeof path, "%s/amiibo-key.bin", fuzz_workdir());
            FILE *f = fopen(path, "wb");
            fwrite(d, 1, n, f);
            fclose(f);
            AmiiboKeys k;
            if (amiibo_keys_load(path, &k) == 0) { /* a key file of any content derives keys for a dump */
                uint8_t t[AMIIBO_TAG_SIZE] = {0}, p[AMIIBO_TAG_SIZE];
                amiibo_tag_unpack(&k, t, p);
                amiibo_tag_pack(&k, p, t);
            }
            AmiiboLib l;
            amiibo_lib_init(&l);
            amiibo_lib_load_file(&l, path);
            amiibo_lib_free(&l);
            break;
        }
        default: {
            uint8_t uid[7] = {4, 1, 2, 3, 4, 5, 6};
            char dir[600], file[700], uidhex[15];
            snprintf(dir, sizeof dir, "%s/amiibo-store", fuzz_workdir());
            mkdir(dir, 0755);
            amiibo_uid_to_hex(uid, uidhex);
            snprintf(file, sizeof file, "%s/%s.json", dir, uidhex);
            FILE *f = fopen(file, "wb");
            fwrite(d, 1, n, f);
            fclose(f);
            Amiibo a;
            amiibo_store_load(dir, uid, &a);
            uint8_t id[8] = {1, 1, 0, 0, 0, 4, 0, 2}, uids[4][7];
            amiibo_store_find_by_id(dir, id, &a);
            amiibo_store_list(dir, uids, 4);
            amiibo_store_delete(dir, uid);
            break;
        }
    }
    free(z);
    return 0;
}

static void add_mode(FuzzSeedFn add, void *ctx, unsigned mode, const void *p, size_t n) {
    uint8_t *b = malloc(n + 1);
    b[0] = (uint8_t)mode;
    memcpy(b + 1, p, n);
    add(ctx, b, n + 1);
    free(b);
}
void fuzz_amiibo_seeds(FuzzSeedFn add, void *ctx) {
    uint8_t id[8] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x02};
    Amiibo a;
    amiibo_new(&a, id, 42);
    uint8_t mii[AMIIBO_MII_CHARINFO];
    memset(mii, 7, sizeof mii);
    amiibo_set_register(&a, "Player", mii, (AmiiboDate){2026, 10, 27});
    uint8_t area[16] = "savegamedata....";
    amiibo_area_create(&a, 0x1234, 0x0100000000010000ULL, area, sizeof area, 0xaa);
    static char json[16384];
    size_t m = amiibo_to_json(&a, json, sizeof json);
    add_mode(add, ctx, 0, json, m);
    static const char api[] =
        "{\"amiibo\":[{\"head\":\"01010000\",\"tail\":\"00040002\",\"name\":\"Mario\",\"character\":\"Mario\",\"gameSeries\":\"Super Mario\","
        "\"amiiboSeries\":\"Super Mario Bros.\",\"type\":\"Figure\",\"image\":\"https:\\/\\/example.invalid\\/icon.png\"},"
        "{\"head\":\"01030000\",\"tail\":\"00520002\",\"name\":\"Link \\u00e9\",\"character\":\"Link\",\"gameSeries\":\"Zelda\",\"amiiboSeries\":\"Zelda\",\"type\":\"Card\"}]}";
    static const char db[] =
        "{\"amiibos\":{\"0x0101000000040002\":{\"name\":\"Mario\"},\"0x0103000000520002\":{\"name\":\"Link\"}},\"characters\":{\"0x0000\":{\"name\":\"Mario\"}},"
        "\"amiibo_series\":{\"0x00\":{\"name\":\"Super Mario Bros.\"}},\"game_series\":{\"0x000\":{\"name\":\"Super Mario\"}},\"types\":{\"0x00\":{\"name\":\"Figure\"}}}";
    static const char bare[] = "{\"0x0101000000040002\":{\"name\":\"Mario\"},\"0x0103000000520002\":{\"name\":\"Link\"}}";
    add_mode(add, ctx, 1, api, sizeof api - 1);
    add_mode(add, ctx, 1, db, sizeof db - 1);
    add_mode(add, ctx, 1, bare, sizeof bare - 1);
    add_mode(add, ctx, 2, api, sizeof api - 1);
    add_mode(add, ctx, 2, json, m);
    add_mode(add, ctx, 3, "SGVsbG8gYW1paWJvbw==", 20);
    add_mode(add, ctx, 3, "0101000000040002", 16);
    uint8_t plain[AMIIBO_TAG_SIZE], tag[AMIIBO_TAG_SIZE];
    AmiiboKeys k;
    test_keys(&k);
    amiibo_to_plain(&a, plain);
    amiibo_export_dump(&k, &a, tag);
    add_mode(add, ctx, 4, tag, sizeof tag);
    add_mode(add, ctx, 5, plain, sizeof plain);
    add_mode(add, ctx, 6, &k, 160);
}
FUZZ_TARGET(amiibo);
