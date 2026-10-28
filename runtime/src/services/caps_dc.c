/* caps:dc (nn::capsrv IDecoderControlService): the JPEG decoder the album and the applets' image loaders call. 3001 DecodeJpeg takes u32 width,
 * u32 height and a 0x20-byte ScreenShotDecodeOption as raw data, the JPEG in input buffer 0 (type 0x45) and writes width x height RGBA8 into output
 * buffer 0 (type 0x46) through jpeg_core (the same decoder as nvjpg and caps:u's album). A file of another size is box-scaled to the requested one
 * (the console's exact behaviour there is not known); the option flags are ignored. Layout from the public documentation, unverified against a title.
 * Registered by a constructor so services.c needs no edit; the registry is a plain static table that is valid before main. */
#include "svc_common.h"
#include "hz_result.h"
#include "jpeg_core.h"

static void dc_decode_jpeg(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 8) {
        c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
        return;
    }
    int w = (int)IPC_IN(c, uint32_t, 0), h = (int)IPC_IN(c, uint32_t, 4);
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    const uint8_t *jpg = in && in->size ? ipc_buf_ptr(in) : NULL;
    uint8_t *dst = out && out->size ? ipc_buf_ptr(out) : NULL;
    if (!jpg || !dst || w <= 0 || h <= 0) {
        c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
        return;
    }
    int rc = napjpeg_decode_into(jpg, (size_t)in->size, &w, &h, dst, (size_t)out->size);
    if (rc != JPEGCORE_OK) {
        LOG("caps", "caps:dc DecodeJpeg: %u-byte file to %dx%d failed (jpeg_core %d)", (unsigned)in->size, w, h, rc);
        c->result = HZ_R_CAPTURE_INVALID_ARGUMENT;
    }
}
static const IpcCmd dc_cmds[] = {
    {3001, "DecodeJpeg", dc_decode_jpeg, IPC_IMPL},
};
static IpcObject *dc_open(void) { return IPC_OBJECT("caps:dc", dc_cmds, NULL, NULL); }
__attribute__((constructor)) static void caps_dc_register(void) { ipc_register_service("caps:dc", dc_open); }
