/* Unit tests: ngc:u (profanity filter): clean by default, real matching with a user-supplied word list. */
#include "../src/services/ngc.c"
#include "svc_test.h"
#include "romfs_fixture.h"
#include <sys/stat.h>
#include <unistd.h>

static void utf16(uint16_t *o, const char *s) { while ((*o++ = (uint8_t)*s++)) {} }

int main(void) {
    IpcObject *o = ngc_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 4, "GetContentVersion");
    uint32_t v0 = SVC_OUT(&c, uint32_t, 0);
    uint16_t text[32]; utf16(text, "hello Darn World");
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, text, 16 * 2);
    svc_call_c(o, 1, &c); CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 0, "Check: clean without a list");
    uint16_t out[32] = {0};
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, text, 16 * 2); svc_set_out_buf(&c, out, sizeof out);
    svc_call_c(o, 2, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0 && memcmp(out, text, 32) == 0, "Mask: text unchanged without a list");

    const char *list = "# comment\ndarn\r\nWORLD\n";
    CHECK(ngc_words_load_text(&words, list, strlen(list)) == 2, "two words parsed, comment skipped");
    words_loaded = 1;
    CHECK(words.version > v0, "list load bumps the content version");
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, text, 16 * 2);
    svc_call_c(o, 1, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "Check flags listed text (case-insensitive)");
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, text, 16 * 2); svc_set_out_buf(&c, out, sizeof out);
    svc_call_c(o, 2, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 9, "Mask counts masked units");
    uint16_t want[32]; utf16(want, "hello **** *****");
    CHECK(memcmp(out, want, 32) == 0, "Mask replaces matches with asterisks");
    uint16_t clean[8]; utf16(clean, "fine");
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, clean, 8);
    svc_call_c(o, 1, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "Check: clean text stays clean with a list");
    CHECK(ngc_check(&words, NULL, 0) == 0 && ngc_mask(&words, NULL, 0) == 0, "empty text");
    { /* ipc-local-stubs: the word lists come from the firmware's NgWord archive, and Check2 / Mask2 answer like Check / Mask */
        char tmp[200]; snprintf(tmp, sizeof tmp, "%s/svc_ngc.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp"); CHECK(mkdtemp(tmp) != NULL, "temp dir");
        /* UTF-16BE with a BOM: ".*darn.*", ".*a\\$\\$.*", a line with regular-expression syntax, a long line, a CRLF line */
        const char *lines = ".*darn.*\n.*a\\$\\$.*\n.*a[bc]d.*\n.*abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz.*\n.*bad word.*\r\n.*DUPE.*\n.*dupe.*\n";
        uint8_t txt[512]; size_t tn = 0; txt[tn++] = 0xfe; txt[tn++] = 0xff;
        for (const char *q = lines; *q; q++) { txt[tn++] = 0; txt[tn++] = (uint8_t)*q; }
        NgcWords w; memset(&w, 0, sizeof w);
        CHECK(ngc_words_add_ngword(&w, txt, tn) == 4 && w.count == 4, "four of the seven lines are plain words (got %u)", w.count);
        uint16_t t1[32], t2[32], t3[32], t4[32]; utf16(t1, "Oh DARN it"); utf16(t2, "a$$ and"); utf16(t3, "abd"); utf16(t4, "bad word!");
        CHECK(ngc_check(&w, t1, 10) == 1 && ngc_check(&w, t2, 7) == 1 && ngc_check(&w, t3, 3) == 0 && ngc_check(&w, t4, 9) == 1, "the escaped and the spaced word match, the regex line was skipped");
        CHECK(ngc_words_add_ngword(&w, txt, 1) == 0 && ngc_words_add_ngword(&w, (const uint8_t *)"\xfe\xff", 2) == 0, "empty and truncated input add nothing");

        char sysd[260]; snprintf(sysd, sizeof sysd, "%s/sysdata", tmp); mkdir(sysd, 0755);
        uint8_t img[0x1100]; size_t isz = romfs_fixture_one_file(img, "common.txt", txt, tn);
        char arch[300]; snprintf(arch, sizeof arch, "%s/0100000000000806.bin", sysd);
        FILE *af = fopen(arch, "wb"); fwrite(img, 1, isz, af); fclose(af);
        NgcWords fw; memset(&fw, 0, sizeof fw);
        CHECK(ngc_words_load_firmware(&fw, arch) == 4, "ngc_words_load_firmware reads common.txt out of the RomFS image");
        char missing[300]; snprintf(missing, sizeof missing, "%s/none.bin", sysd);
        CHECK(ngc_words_load_firmware(&fw, missing) == 0, "no archive: no words");

        static char dd[260]; snprintf(dd, sizeof dd, "%s/build", tmp); mkdir(dd, 0755); g_cfg.data_dir = dd;
        unsetenv("NAPPLE_NGC_WORDLIST");
        memset(&words, 0, sizeof words); words_loaded = 0;
        svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0, "GetContentVersion (the archive sits one level above the data dir)");
        uint32_t v1 = SVC_OUT(&c, uint32_t, 0);
        CHECK(words.count == 4, "the firmware list was loaded for the title (%u words)", words.count);
        memset(&c, 0, sizeof c); svc_set_in_buf(&c, t1, 20); svc_call_c(o, 4, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 1, "4 Check2 flags a listed word");
        uint16_t o2[32] = {0}; memset(&c, 0, sizeof c); svc_set_in_buf(&c, t1, 20); svc_set_out_buf(&c, o2, sizeof o2); svc_call_c(o, 5, &c);
        uint16_t want2[32]; utf16(want2, "Oh **** it");
        CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 4 && !memcmp(o2, want2, 20), "5 Mask2 masks it");
        CHECK(svc_find(o, 4)->status == IPC_IMPL && svc_find(o, 5)->status == IPC_IMPL, "Check2 and Mask2 are real now");
        svc_call(o, 3, NULL, 0, &c); CHECK(c.result == 0, "3 Reload re-reads the archive");
        svc_call(o, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) > v1 && words.count == 4, "...the content version moved and the list is the same");
        setenv("NAPPLE_NGC_WORDLIST", "/nonexistent/napple-wordlist", 1);
        svc_call(o, 3, NULL, 0, &c); CHECK(words.count == 0, "a user word list (NAPPLE_NGC_WORDLIST) wins over the firmware's, even when it is empty");
        unsetenv("NAPPLE_NGC_WORDLIST");
        /* the real archive of the Testing Material build, when this machine has one */
        const char *real = "/Users/neto/NappleWork/games/0100000000010000/sysdata/0100000000000806.bin";
        if (access(real, R_OK) == 0) {
            NgcWords *rw = calloc(1, sizeof *rw); unsigned n = ngc_words_load_firmware(rw, real);
            CHECK(n >= 500 && n == rw->count, "the real firmware NgWord archive gives %u words", n);
            unsigned bad = 0; for (unsigned i = 0; i < rw->count; i++) if (!rw->len[i] || rw->len[i] > NGC_MAX_WORD_LEN) bad++;
            CHECK(bad == 0, "every loaded word has a valid length");
            CHECK(ngc_check(rw, rw->w[n / 2], rw->len[n / 2]) >= 1, "a word of the list is flagged by the list");
            printf("real NgWord archive: %u words\n", n); free(rw);
        }
        char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp); if (system(cmd)) printf("cleanup failed\n");
        g_cfg.data_dir = NULL;
    }
    printf("{\"test\":\"svc_ngc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
