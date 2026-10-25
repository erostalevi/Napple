/* ios_dylib.c on any host: dylib naming and Mach-O header validation against a header built the way crates/napple-build/src/macho.rs does. */
#include "../check.h"
#include "ios_dylib.h"
#include <string.h>

static void seg(uint8_t **p, const char *name, uint64_t vm, uint64_t size, uint32_t prot) {
    uint32_t v[2] = {0x19, 72}; memcpy(*p, v, 8);
    char n[16] = {0}; strcpy(n, name); memcpy(*p + 8, n, 16);
    uint64_t q[4] = {vm, size, vm, size}; memcpy(*p + 24, q, 32);
    uint32_t pr[4] = {prot, prot, 0, 0}; memcpy(*p + 56, pr, 16);
    *p += 72;
}

int main(void) {
    char out[256];
    CHECK(ios_dylib_path(out, sizeof out, "/var/App/Game.app/game/0123456789abcdef/modules", "main") == 0, "line %d", __LINE__);
    CHECK(!strcmp(out, "/var/App/Game.app/Frameworks/0123456789abcdef.main.dylib"), "dylib path %s", out);
    CHECK(ios_dylib_path(out, sizeof out, "/var/App/Game.app/game/0123456789abcdef/nro", "ab12") == 0, "line %d", __LINE__);
    CHECK(!strcmp(out, "/var/App/Game.app/Frameworks/0123456789abcdef.nro.ab12.dylib"), "dylib path %s", out);
    CHECK(ios_dylib_path(out, 20, "/var/App/Game.app/game/0123456789abcdef/modules", "main") == -1, "line %d", __LINE__);
    CHECK(ios_dylib_path(out, sizeof out, "modules", "main") == -1, "line %d", __LINE__);

    /* text_end 0x3f00, data_off 0x8080 (ro_end 0x8000), key 0x10000, patch 0x14000, size 0x18000 (file 0x14040) */
    static uint8_t buf[0x4000]; memset(buf, 0, sizeof buf);
    uint32_t hdr[8] = {0xfeedfacf, 0x0100000c, 0, 6, 4, 4 * 72, 0, 0}; memcpy(buf, hdr, 32);
    uint8_t *p = buf + 32;
    seg(&p, "__TEXT", 0, 0x4000 + 0x4000, 5);
    seg(&p, "__RODATA", 0x8000, 0x4000, 1);
    seg(&p, "__DATA", 0x4000 + 0x8000, 0x14000 - 0x8000, 3);
    seg(&p, "__TEXT_EXEC", 0x4000 + 0x14000, 0x4000, 5);
    uint32_t n = 5; memcpy(buf + 16, &n, 4);   /* ncmds covers an extra, empty slot */
    uint32_t four = 4; memcpy(buf + 16, &four, 4);
    CHECK(ios_dylib_check(buf, 0x3f00, 0x8080, 0x10000, 0x14000, 0x14040) == NULL, "line %d", __LINE__);
    CHECK(ios_dylib_check(buf, 0x3f00, 0x8080, 0x10000, 0x18000, 0x14040) != NULL, "line %d", __LINE__);   /* patch area elsewhere */
    CHECK(ios_dylib_check(buf, 0x5000, 0x8080, 0x10000, 0x14000, 0x14040) != NULL, "line %d", __LINE__);   /* text longer than __TEXT */
    buf[0] = 0;
    CHECK(ios_dylib_check(buf, 0x3f00, 0x8080, 0x10000, 0x14000, 0x14040) != NULL, "line %d", __LINE__);   /* bad magic */
    printf("{\"test\":\"ios_dylib\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
