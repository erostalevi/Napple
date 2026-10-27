/* Unit tests: ngc:u (profanity filter): clean by default, real matching with a user-supplied word list. */
#include "../src/services/ngc.c"
#include "svc_test.h"

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
    printf("{\"test\":\"svc_ngc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
