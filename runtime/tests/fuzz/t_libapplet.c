/* Fuzz target: the library applet argument parsers (services/applet_lib_core.c): the error applet's header/argument storages, the software keyboard config
 * and its inline request protocol, controller support, player select and the environment-variable policies. The storages are bytes a guest pushes.
 * Input: [mode][bytes]. */
#include "../../src/services/applet_lib_core.h"
#include "fuzz.h"
#include <stdlib.h>
#include <string.h>

int fuzz_libapplet(const uint8_t *d, size_t n) {
    if (!n) return 0;
    unsigned mode = d[0] % 6;
    d++, n--;
    char *z = malloc(n + 1); /* NUL-terminated copy for the C-string entry points */
    memcpy(z, d, n);
    z[n] = 0;
    char buf[256];
    switch (mode) {
        case 0: { /* error applet: the header is the first bytes, the argument storage the rest */
            size_t h = n ? d[0] % (n + 1) : 0;
            AppletErrorInfo e;
            applet_error_parse(d, h, d + h, n - h, &e);
            applet_error_describe(&e, buf, sizeof buf);
            applet_error_code_text(&e, buf, 16);
            applet_error_code_text(&e, buf, sizeof buf);
            applet_error_notify(&e, buf, buf);
            break;
        }
        case 1: { /* swkbd config + fitting typed text to it */
            AppletSwkbdConfig c;
            applet_swkbd_parse(d, n, &c);
            applet_swkbd_parse(NULL, 0, &c);
            applet_swkbd_parse(d, n, &c);
            char out[64];
            applet_swkbd_fit(&c, z, out, sizeof out);
            applet_swkbd_fit(&c, z, out, 1);
            break;
        }
        case 2: { /* swkbd inline mode: a sequence of request storages, 2-byte length prefixed */
            AppletSwkbdConfig c;
            applet_swkbd_parse(d, n > 0x4c8 ? 0x4c8 : n, &c);
            AppletSwkbdInline k;
            applet_swkbd_inline_init(&k, &c);
            LibAppletCall call;
            memset(&call, 0, sizeof call);
            FzIn r = {d, n, 0};
            for (int i = 0; i < 8 && fz_left(&r) >= 2; i++) {
                size_t len = fz_u16(&r);
                if (len > fz_left(&r)) len = fz_left(&r);
                applet_swkbd_inline_request(&k, d + r.at, len, i & 1 ? "typed text" : NULL, &call);
                r.at += len;
            }
            libapplet_call_free(&call);
            break;
        }
        case 3: { /* controller support: argument header, connected pads, policy strings */
            AppletControllerAnswer a;
            applet_controller_answer(d, n, n ? d[n - 1] % 9 - 1 : 1, &a);
            uint8_t out[APPLET_CONTROLLER_OUT_SIZE];
            applet_controller_encode(&a, out);
            uint32_t res = 0;
            applet_controller_apply_policy(z, &a, &res);
            applet_controller_encode_result(&a, res, out);
            break;
        }
        case 4: { /* player select: profile pick by index / spec */
            uint8_t uids[8][16];
            memset(uids, 7, sizeof uids);
            int idx = 0;
            int cancel = applet_player_select_spec(z, &idx);
            FzIn r = {d, n, 0};
            unsigned count = fz_u8(&r) % 10;
            applet_player_select_pick((const uint8_t(*)[16])uids, count > 8 ? 8 : count, (int)fz_u32(&r), cancel);
            uint8_t out[APPLET_PLAYER_SELECT_OUT_SIZE];
            applet_player_select_answer(uids[0], cancel, out);
            break;
        }
        default: { /* firmware applet policy strings and the applet id table */
            FzIn r = {d, n, 0};
            const LibAppletProgram *p = libapplet_program_for(fz_u32(&r));
            size_t half = n / 2;
            char *a = strndup(z, half), *b = strdup(z + half);
            if (p) libapplet_firmware_enabled(p, a, b);
            libapplet_firmware_enabled(p, a, NULL);
            libapplet_firmware_enabled(p, NULL, b);
            libapplet_mode_is_inline(fz_u32(&r));
            free(a);
            free(b);
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
void fuzz_libapplet_seeds(FuzzSeedFn add, void *ctx) {
    uint8_t err[64] = {8, 1, 0, 0};
    fz_put32(err + 4, 2162), fz_put32(err + 8, 4);
    add_mode(add, ctx, 0, err, 24);
    uint8_t cfg[0x4c8 + 16] = {0};
    fz_put32(cfg + 0x3ac, 10), fz_put32(cfg + 0x3b0, 3), cfg[0x3b4] = 1;
    add_mode(add, ctx, 1, cfg, sizeof cfg);
    /* inline requests: Finalize, Calc (0xA), SetUserWord, an unknown command */
    uint8_t inl[4 * 12 + 2 * 4];
    size_t at = 0;
    static const uint32_t cmds[] = {0xA, 0x6, 0x4, 0x99};
    for (unsigned i = 0; i < 4; i++) {
        fz_put16(inl + at, 8), at += 2;
        fz_put32(inl + at, cmds[i]), at += 4;
        fz_put16(inl + at, 0), at += 2;
    }
    add_mode(add, ctx, 2, inl, at);
    uint8_t ctl[16] = {2, 4, 0, 1, 0, 0, 1, 3};
    add_mode(add, ctx, 3, ctl, sizeof ctl);
    add_mode(add, ctx, 3, "cancel", 6);
    add_mode(add, ctx, 3, "strict", 6);
    add_mode(add, ctx, 4, "cancel", 6);
    add_mode(add, ctx, 4, "1\3\0\0\0", 5);
    add_mode(add, ctx, 5, "\x0c\0\0\0all0x11,0x0c", 17);
    add_mode(add, ctx, 5,
             "\x11\0\0\0"
             "0swkbd,controller,error",
             23);
}
FUZZ_TARGET(libapplet);
