/* Unit tests: nvdrv multimedia engines (nvdrv_media_core behind nvdrv.c): host1x command streams submitted through the real ioctl handlers,
 * NVDEC with the null backend and a recording backend, VIC colour conversion, NVJPG decode (synthetic JPEGs), MSENC, relocations, gathers,
 * malformed streams, unmapped surfaces. Register numbers come from nvdrv_media_regs.h (unverified against real traces). */
#include "../src/services/nvdrv.c"
#include "../src/services/nvdrv_media_regs.h"
#include "../src/services/codec_headers_core.h"
#include "../src/services/jpeg_core.h"
#include "check.h"
#include "codec_vectors.h"
#include <math.h>

#ifdef NAPPLE_HOST_TEST /* make test-host: kernel and thread stand-ins of tests/host */
#include "host/host_stubs.h"
#elif defined(__linux__) /* tools/linux-check: ipc_mock.c has everything but the summary hook */
void summary_ipc(const char *service, unsigned cmd, int status, unsigned request_bytes) { (void)service; (void)cmd; (void)status; (void)request_bytes; }
#endif

static IpcObject *nv;
static IpcCall call;
static uint8_t io[0x4000];
static uint32_t args[2];
static uint32_t cmd_id(unsigned dir, unsigned size, unsigned type, unsigned nr) { return (dir << 30) | (size << 16) | (type << 8) | nr; }
static uint32_t ioctl_(uint32_t fd, uint32_t cmd, void *data, uint32_t size) {
    memset(&call, 0, sizeof call);
    args[0] = fd; args[1] = cmd;
    call.object = nv; call.in = (const uint8_t *)args; call.in_size = 8;
    memcpy(io, data, size);
    call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)io, size, 0};
    call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)io, size, 0};
    nv_ioctl(nv, &call);
    memcpy(data, io, size);
    uint32_t err; memcpy(&err, call.out + call.out_size - 4, 4);
    return err;
}
static uint32_t open_(const char *path) {
    memset(&call, 0, sizeof call);
    static char p[64]; snprintf(p, sizeof p, "%s", path);
    call.object = nv; call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)p, strlen(p), 0};
    nv_open(nv, &call);
    uint32_t fd; memcpy(&fd, call.out, 4); return fd;
}
static void close_(uint32_t fd) { memset(&call, 0, sizeof call); args[0] = fd; call.object = nv; call.in = (const uint8_t *)args; call.in_size = 4; nv_close(nv, &call); }

typedef struct { uint32_t handle, iova; uint8_t *mem; uint32_t size; } Buf;
static uint32_t nvmap_fd;
/* nvmap handle backed by host memory, pinned through MAP_CMD_BUFFER on `chan`. */
static Buf mkbuf(uint32_t chan, uint32_t size) {
    Buf b = {0}; b.size = size; b.mem = aligned_alloc(4096, (size + 4095) & ~4095u); memset(b.mem, 0, size);
    uint32_t create[2] = {size, 0}; ioctl_(nvmap_fd, cmd_id(3, 8, 1, 0x01), create, 8); b.handle = create[1];
    uint8_t al[0x20] = {0}; uint64_t addr = (uintptr_t)b.mem; memcpy(al, &b.handle, 4); memcpy(al + 12, &(uint32_t){4096}, 4); memcpy(al + 24, &addr, 8);
    ioctl_(nvmap_fd, cmd_id(3, 0x20, 1, 0x04), al, 0x20);
    uint32_t mb[5] = {1, 0, 0, b.handle, 0}; ioctl_(chan, cmd_id(3, 20, 0, 0x09), mb, sizeof mb); b.iova = mb[4];
    return b;
}
/* A SUBMIT of one command buffer (words in `cb`) with optional relocations {cmdbuf offset, target handle, target offset, shift}. */
typedef struct { uint32_t at, target, toff, shift; } RelocSpec;
static uint32_t submit(uint32_t chan, Buf *cb, const uint32_t *words, uint32_t n, const RelocSpec *rel, uint32_t nrel) {
    memcpy(cb->mem, words, (size_t)n * 4);
    uint8_t sub[16 + 12 + 16 * 4 + 4 * 4] = {0};
    uint32_t hdr[4] = {1, nrel, 0, 0}; memcpy(sub, hdr, 16);
    uint32_t c[3] = {cb->handle, 0, n}; memcpy(sub + 16, c, 12);
    for (uint32_t i = 0; i < nrel; i++) {
        uint32_t r[4] = {cb->handle, rel[i].at, rel[i].target, rel[i].toff}; memcpy(sub + 28 + i * 16, r, 16);
        memcpy(sub + 28 + nrel * 16 + i * 4, &rel[i].shift, 4);
    }
    return ioctl_(chan, cmd_id(3, 16, 0, 0x01), sub, 28 + nrel * 20);
}
#define SETCL(cls) ((0u << 28) | ((cls) << 6))
#define INCR(off, n) ((1u << 28) | ((off) << 16) | (n))
#define NONINCR(off, n) ((2u << 28) | ((off) << 16) | (n))
#define IMM(off, v) ((4u << 28) | ((off) << 16) | (v))
#define GATHER(n) ((6u << 28) | (n))
#define GATHER_REG(off, incr, n) ((6u << 28) | ((off) << 16) | (1u << 15) | ((incr) << 14) | (n))
static const MediaStats *stats(uint32_t fd) { return media_engine_stats(fds[fd].ch.eng); }
/* VIC: a flat NV12 picture with rows of `pitch` bytes, and the config structure's output surface word (+0x20). */
static void vic_fill(Buf *y, Buf *c, int w, int h, int pitch, uint8_t Y, uint8_t U, uint8_t V) {
    memset(y->mem, 0, y->size); memset(c->mem, 0, c->size);
    for (int r = 0; r < h; r++) memset(y->mem + r * pitch, Y, (size_t)w);
    for (int r = 0; r < (h + 1) / 2; r++) for (int x = 0; x < w; x += 2) { c->mem[r * pitch + x] = U; c->mem[r * pitch + x + 1] = V; }
}
static void vic_cfg(Buf *cfg, uint32_t fmt, uint32_t kind, uint32_t bh, uint32_t w, uint32_t h) {
    memset(cfg->mem, 0, VIC_CFG_BYTES);
    uint64_t oc = fmt | ((uint64_t)kind << 11) | ((uint64_t)bh << 15) | ((uint64_t)(w - 1) << 32) | ((uint64_t)(h - 1) << 46);
    memcpy(cfg->mem + VIC_CFG_OUTPUT_SURFACE, &oc, 8);
}

/* A backend that records what it is asked and produces a 16x16 frame. */
static struct { int opened, closed, decoded, codec, resets, configures, refuse; DecodeRequest last; DecoderConfig cfg; uint8_t pic_head[8]; } rec;
static void *rec_open(int codec) { rec.opened++; rec.codec = codec; return &rec; }
static void rec_close(void *c) { (void)c; rec.closed++; }
static int rec_decode(void *c, const DecodeRequest *r, DecodeResult *o) {
    (void)c; rec.decoded++; rec.last = *r; if (r->pic) memcpy(rec.pic_head, r->pic, 8);
    memset(r->luma, 0x55, 16 * 16); memset(r->chroma, 0x66, 16 * 8); *o = (DecodeResult){16, 16, 16, 0, 0}; return 0;
}
static void rec_reset(void *c) { (void)c; rec.resets++; }
static int rec_configure(void *c, const DecoderConfig *cfg) { (void)c; rec.configures++; rec.cfg = *cfg; return rec.refuse ? -1 : 0; }
static const DecoderBackend rec_backend = {"recording", rec_open, rec_close, rec_decode, rec_reset, rec_configure, DECODER_BACKEND_ABI};

static double psnr(const uint8_t *a, const uint8_t *b, size_t n4) {
    double se = 0; size_t k = 0;
    for (size_t i = 0; i < n4; i++) if (i % 4 != 3) { double d = (double)a[i] - b[i]; se += d * d; k++; }
    return se == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / (se / (double)k));
}

int main(void) {
#ifdef NAPPLE_HOST_TEST
    host_thread_attach(calloc(1, 0x200));
#endif
    { /* the engines read guest memory through guest_ptr: the runtime build's one only accepts the guest address space, not this process's heap */
        static uint8_t probe[64];
        if (!guest_ptr((uintptr_t)probe, sizeof probe)) {
            printf("{\"test\":\"svc_nvdrv_media\",\"skipped\":\"guest_ptr rejects host memory in this build; run make test-host or tools/linux-check\"}\n");
            return 0;
        }
    }
    decoder_backend_install(NULL); /* the runtime build installs the VideoToolbox backend at startup (decoder_vt.c); these checks are about the null backend */
    nv = nv_new();
    nvmap_fd = open_("/dev/nvmap");
    uint32_t dec = open_("/dev/nvhost-nvdec"), vic = open_("/dev/nvhost-vic"), jpg = open_("/dev/nvhost-nvjpg"), enc = open_("/dev/nvhost-msenc");
    CHECK(fds[dec].ch.eng && fds[vic].ch.eng && fds[jpg].ch.eng && fds[enc].ch.eng, "every media channel has an engine");
    uint32_t ctrl = open_("/dev/nvhost-ctrl"); CHECK(!fds[ctrl].ch.eng && !fds[nvmap_fd].ch.eng, "nvmap and ctrl have none");

    /* ---- NVDEC with the null backend ---- */
    Buf cb = mkbuf(dec, 0x1000), bs = mkbuf(dec, 0x2000), pic = mkbuf(dec, 0x1000), luma = mkbuf(dec, 0x4000), chroma = mkbuf(dec, 0x2000), st = mkbuf(dec, 0x100);
    CHECK(cb.iova && bs.iova && luma.iova && cb.iova != bs.iova, "buffers pinned at distinct device addresses");
    memcpy(bs.mem, h264_64x48, sizeof h264_64x48);
    uint32_t len = sizeof h264_64x48; memcpy(pic.mem + 18 * 4, &len, 4);
    uint32_t prog[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264,
                       INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 0,
                       INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8,
                       INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8,
                       IMM(NVM_EXECUTE, 0)};
    CHECK(submit(dec, &cb, prog, sizeof prog / 4, NULL, 0) == NV_OK, "NVDEC submit with an H.264 frame");
    CHECK(stats(dec)->executes == 1 && stats(dec)->frames == 1 && stats(dec)->errors == 0 && stats(dec)->unknown_regs == 0, "one execute, one frame, no errors (exec %llu frames %llu err %llu unk %llu)",
          (unsigned long long)stats(dec)->executes, (unsigned long long)stats(dec)->frames, (unsigned long long)stats(dec)->errors, (unsigned long long)stats(dec)->unknown_regs);
    uint32_t sw[8]; memcpy(sw, st.mem, 32);
    CHECK(sw[3] == 0 && sw[4] == 64 && sw[5] == 48 && sw[0] == 12, "status structure: error 0, 64x48, 12 macroblocks (%u %u %u %u)", sw[3], sw[4], sw[5], sw[0]);
    int flat = 1; for (int i = 1; i < 64 * 48; i++) flat &= luma.mem[i] == luma.mem[0];
    CHECK(flat && luma.mem[0] >= 32 && chroma.mem[0] == 128 && chroma.mem[64 * 24 - 1] == 128, "NV12 output written: luma %u, chroma 128", luma.mem[0]);
    uint8_t first = luma.mem[0];
    bs.mem[sizeof h264_64x48 - 1] ^= 0xff; /* a different frame content */
    prog[2] = DEC_CODEC_H264;
    CHECK(submit(dec, &cb, prog, sizeof prog / 4, NULL, 0) == NV_OK && luma.mem[0] != first && stats(dec)->frames == 2, "a different bitstream gives a different picture");
    memset(luma.mem, 0, 0x4000);

    /* high profile / cropping sizes come from the SPS */
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, h264_50x30_crop, sizeof h264_50x30_crop); len = sizeof h264_50x30_crop; memcpy(pic.mem + 18 * 4, &len, 4);
    submit(dec, &cb, prog, sizeof prog / 4, NULL, 0); memcpy(sw, st.mem, 32);
    CHECK(sw[4] == 50 && sw[5] == 30, "cropped H.264 stream reports 50x30");

    /* VP9: key frame sizes the surface, the following inter frame reuses it */
    uint32_t vp9prog[sizeof prog / 4]; memcpy(vp9prog, prog, sizeof prog); vp9prog[2] = DEC_CODEC_VP9;
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_key_70x38, sizeof vp9_key_70x38); len = sizeof vp9_key_70x38; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 12 * 4, &len, 4);
    CHECK(submit(dec, &cb, vp9prog, sizeof vp9prog / 4, NULL, 0) == NV_OK, "VP9 key frame"); memcpy(sw, st.mem, 32);
    CHECK(sw[3] == 0 && sw[4] == 70 && sw[5] == 38, "VP9 key frame: 70x38 (%u x %u)", sw[4], sw[5]);
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_inter, sizeof vp9_inter); len = sizeof vp9_inter; memcpy(pic.mem + 12 * 4, &len, 4);
    submit(dec, &cb, vp9prog, sizeof vp9prog / 4, NULL, 0); memcpy(sw, st.mem, 32);
    CHECK(sw[3] == 0 && sw[4] == 70 && sw[5] == 38, "VP9 inter frame reuses the size");
    /* H.265 */
    uint32_t hprog[sizeof prog / 4]; memcpy(hprog, prog, sizeof prog); hprog[2] = DEC_CODEC_H265;
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, h265_96x64, sizeof h265_96x64); len = sizeof h265_96x64; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 12 * 4, &len, 4);
    submit(dec, &cb, hprog, sizeof hprog / 4, NULL, 0); memcpy(sw, st.mem, 32);
    CHECK(sw[3] == 0 && sw[4] == 96 && sw[5] == 64, "H.265 stream: 96x64 from the SPS (%u x %u)", sw[4], sw[5]);
    /* unsupported codec: error in the status, submit still completes */
    uint32_t badcodec[sizeof prog / 4]; memcpy(badcodec, prog, sizeof prog); badcodec[2] = 77;
    uint64_t errs = stats(dec)->errors;
    CHECK(submit(dec, &cb, badcodec, sizeof badcodec / 4, NULL, 0) == NV_OK && stats(dec)->errors == errs + 1, "unsupported codec: counted, submit completes"); memcpy(sw, st.mem, 32);
    CHECK(sw[3] == 1, "unsupported codec reported in the status structure");

    /* picture slots: output goes to the slot's surfaces when set */
    uint32_t slot[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 2,
                       INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8, INCR(NVDEC_SET_PICTURE_LUMA_OFFSET0 + 2, 1), (luma.iova + 0x1000) >> 8, INCR(NVDEC_SET_PICTURE_CHROMA_OFFSET0 + 2, 1), (chroma.iova + 0x1000) >> 8,
                       IMM(NVM_EXECUTE, 0)};
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, h264_64x48, sizeof h264_64x48); len = sizeof h264_64x48; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 18 * 4, &len, 4);
    uint32_t cur2 = 2u << 2; memcpy(pic.mem + NVDEC_H264_W_FLAGS2 * 4, &cur2, 4); /* curr_pic_idx 2 */
    memset(luma.mem, 0, 0x4000);
    submit(dec, &cb, slot, sizeof slot / 4, NULL, 0);
    CHECK(luma.mem[0] == 0 && luma.mem[0x1000] != 0 && chroma.mem[0x1000] == 128, "curr_pic_idx 2 writes the slot-2 surfaces");
    { /* the frame register (0x103) counts past the 17 slots (MK8D): the slot still comes from curr_pic_idx */
        uint32_t past[sizeof slot / 4]; memcpy(past, slot, sizeof slot); past[6] = 20; memset(luma.mem, 0, 0x4000); errs = stats(dec)->errors;
        CHECK(submit(dec, &cb, past, sizeof past / 4, NULL, 0) == NV_OK && stats(dec)->errors == errs && luma.mem[0x1000] != 0, "frame register 20, curr_pic_idx 2: slot 2, no error");
    }
    memset(pic.mem + NVDEC_H264_W_FLAGS2 * 4, 0, 4);

    /* unmapped output surface: error in status, no crash */
    uint32_t unmapped[sizeof prog / 4]; memcpy(unmapped, prog, sizeof prog); unmapped[10] = 0x7fff0000u >> 8; errs = stats(dec)->errors;
    CHECK(submit(dec, &cb, unmapped, sizeof unmapped / 4, NULL, 0) == NV_OK && stats(dec)->errors == errs + 1, "unmapped output surface: counted as an engine error"); memcpy(sw, st.mem, 32);
    CHECK(sw[3] == 2, "unmapped surface reported in the status (2)");

    /* relocation: the input-buffer word is patched from a (handle, offset, shift) entry */
    uint32_t rprog[sizeof prog / 4]; memcpy(rprog, prog, sizeof prog); rprog[5] = 0xdeadbeef; /* in-buf offset placeholder at word 5 */
    memset(luma.mem, 0, 0x4000);
    RelocSpec rl = {5 * 4, bs.handle, 0, 8};
    CHECK(submit(dec, &cb, rprog, sizeof rprog / 4, &rl, 1) == NV_OK && luma.mem[0] != 0, "relocation resolves the bitstream address");
    RelocSpec rl_bad = {5 * 4, 0x4321, 0, 8};
    CHECK(submit(dec, &cb, rprog, sizeof rprog / 4, &rl_bad, 1) == NV_BAD_PARAMETER, "relocation to an unknown handle fails the submit");
    RelocSpec rl_off = {5 * 4, bs.handle, 0x100, 8}; memset(luma.mem, 0, 0x4000);
    submit(dec, &cb, rprog, sizeof rprog / 4, &rl_off, 1); CHECK(media_engine_reg(fds[dec].ch.eng, NVDEC_SET_IN_BUF_BASE_OFFSET) == (bs.iova + 0x100) >> 8, "relocation adds the target offset and shifts");

    /* GATHER: commands living in another buffer; register gather: data words for one register */
    Buf g = mkbuf(dec, 0x1000);
    uint32_t inner[] = {NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 0,
                        INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8, INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8, IMM(NVM_EXECUTE, 0)};
    memcpy(g.mem, inner, sizeof inner);
    uint32_t gprog[] = {SETCL(MEDIA_CLASS_NVDEC), GATHER(sizeof inner / 4), g.iova};
    memset(luma.mem, 0, 0x4000); memcpy(bs.mem, h264_64x48, sizeof h264_64x48); memcpy(pic.mem + 18 * 4, &(uint32_t){sizeof h264_64x48}, 4);
    CHECK(submit(dec, &cb, gprog, 3, NULL, 0) == NV_OK && luma.mem[0] != 0, "GATHER runs the commands of another buffer");
    uint32_t data[] = {0x11111, 0x22222};
    memcpy(g.mem + 0x800, data, 8);
    uint32_t rgprog[] = {SETCL(MEDIA_CLASS_NVDEC), GATHER_REG(NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET, 1, 2), g.iova + 0x800};
    CHECK(submit(dec, &cb, rgprog, 3, NULL, 0) == NV_OK && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET) == 0x11111 && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_COLOC_DATA_OFFSET) == 0x22222, "register GATHER writes consecutive registers");
    uint32_t ngprog[] = {SETCL(MEDIA_CLASS_NVDEC), GATHER(4), 0x1234000};
    CHECK(submit(dec, &cb, ngprog, 3, NULL, 0) == NV_BAD_PARAMETER, "GATHER of an unmapped address fails the submit");

    /* malformed streams */
    uint32_t trunc[] = {SETCL(MEDIA_CLASS_NVDEC), INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 5), 1, 2};
    CHECK(submit(dec, &cb, trunc, 4, NULL, 0) == NV_BAD_PARAMETER, "INCR running past the command buffer");
    uint32_t badop[] = {0x9u << 28};
    CHECK(submit(dec, &cb, badop, 1, NULL, 0) == NV_BAD_PARAMETER, "unknown host1x opcode");
    uint32_t unk[] = {SETCL(MEDIA_CLASS_NVDEC), IMM(0x1ee, 5), 0xe1000000u, SETCL(0), IMM(0, 1)}; uint64_t u0 = stats(dec)->unknown_regs;
    CHECK(submit(dec, &cb, unk, 5, NULL, 0) == NV_OK && stats(dec)->unknown_regs == u0 + 1 && stats(dec)->last_unknown_reg == 0x1ee && stats(dec)->syncpt_incrs >= 1, "unknown registers are counted and logged, EXTEND and host1x INCR_SYNCPT accepted");
    uint32_t mask[] = {SETCL(MEDIA_CLASS_NVDEC), (3u << 28) | (NVDEC_SET_DRV_PIC_SETUP_OFFSET << 16) | 0x5, 0xaaa, 0xbbb};
    submit(dec, &cb, mask, 4, NULL, 0);
    CHECK(media_engine_reg(fds[dec].ch.eng, NVDEC_SET_DRV_PIC_SETUP_OFFSET) == 0xaaa && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_DRV_PIC_SETUP_OFFSET + 2) == 0xbbb, "MASK writes the set bits");
    uint32_t setcl_mask[] = {SETCL(MEDIA_CLASS_NVDEC) | (NVDEC_SET_HISTORY_OFFSET << 16) | 0x3, 0x31, 0x32};
    submit(dec, &cb, setcl_mask, 3, NULL, 0);
    CHECK(media_engine_reg(fds[dec].ch.eng, NVDEC_SET_HISTORY_OFFSET) == 0x31 && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_HISTORY_OFFSET + 1) == 0x32, "SETCL with a register mask");
    uint32_t foreign[] = {SETCL(MEDIA_CLASS_VIC), IMM(NVM_EXECUTE, 0)}; u0 = stats(dec)->executes;
    submit(dec, &cb, foreign, 2, NULL, 0); CHECK(stats(dec)->executes == u0 && stats(dec)->foreign_writes > 0, "writes to another engine's class do not run this engine");

    /* the recording backend sees the request the engine built */
    decoder_backend_install(&rec_backend);
    uint32_t dec2 = open_("/dev/nvhost-nvdec");
    Buf cb2 = mkbuf(dec2, 0x1000);
    memcpy(bs.mem, h264_64x48, sizeof h264_64x48); memset(pic.mem, 0, 0x100); memcpy(pic.mem, "PICINFO!", 8); len = 100; memcpy(pic.mem + 18 * 4, &len, 4);
    { uint32_t c4 = 4u << 2; memcpy(pic.mem + NVDEC_H264_W_FLAGS2 * 4, &c4, 4); } /* curr_pic_idx 4 */
    uint32_t p2[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 4,
                     INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8, IMM(NVM_EXECUTE, 0)};
    CHECK(submit(dec2, &cb2, p2, sizeof p2 / 4, NULL, 0) == NV_OK, "submit with the recording backend");
    CHECK(rec.opened == 1 && rec.decoded == 1 && rec.codec == DEC_CODEC_H264 && rec.last.bitstream_size == 100 && rec.last.picture_index == 4 &&
              rec.last.pic_size >= 0x1000 && !memcmp(rec.pic_head, "PICINFO!", 8) && rec.last.bitstream == bs.mem && rec.last.luma == luma.mem && rec.last.luma_cap >= 0x4000,
          "backend request: codec %d, stream %zu bytes, picture %u, surfaces mapped", rec.codec, rec.last.bitstream_size, rec.last.picture_index);
    CHECK(luma.mem[0] == 0x55 && chroma.mem[0] == 0x66, "the backend's output lands in the guest surfaces");
    uint32_t p3[sizeof p2 / 4]; memcpy(p3, p2, sizeof p2); p3[2] = DEC_CODEC_VP9; submit(dec2, &cb2, p3, sizeof p3 / 4, NULL, 0);
    CHECK(rec.opened == 2 && rec.closed == 1, "a codec change reopens the decoder");
    close_(dec2); CHECK(rec.closed == 2, "closing the channel closes the backend decoder");
    decoder_backend_install(NULL);

    /* the configure hand-off: parameter sets found in the stream reach the backend once per change, reset first; a refusal stops the decode */
    {
        memset(&rec, 0, sizeof rec); decoder_backend_install(&rec_backend);
        uint32_t d3 = open_("/dev/nvhost-nvdec"); Buf cb3 = mkbuf(d3, 0x1000);
        memcpy(bs.mem, h264_64x48, sizeof h264_64x48); len = sizeof h264_64x48; memcpy(pic.mem + 18 * 4, &len, 4);
        uint32_t p4[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 0,
                         INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8, INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8, IMM(NVM_EXECUTE, 0)};
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 1 && rec.resets == 1 && rec.decoded == 1 && stats(d3)->configures == 1, "H.264: parameter sets configure the backend before the first decode (%d configures, %d resets)", rec.configures, rec.resets);
        CHECK(rec.cfg.codec == DEC_CODEC_H264 && rec.cfg.h264.nsps == 1 && rec.cfg.h264.npps == 1 && rec.cfg.width == 64 && rec.cfg.height == 48 && rec.cfg.h264.sps[0][0] == 0x67 &&
              rec.cfg.h264_annexb_size == 8 + 22 + 5 && !memcmp(rec.cfg.h264_annexb + 4, h264_64x48 + 4, 22), "H.264 config: one SPS, one PPS, 64x48, Annex B form for vth264_config");
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 1 && rec.decoded == 2, "the same parameter sets again: no new configure");
        memcpy(bs.mem, h264_208x112_high, sizeof h264_208x112_high); len = sizeof h264_208x112_high; memcpy(pic.mem + 18 * 4, &len, 4);
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 2 && rec.resets == 2 && rec.cfg.width == 208 && rec.cfg.height == 112, "new parameter sets: reset then configure again (%dx%d)", rec.cfg.width, rec.cfg.height);
        memcpy(bs.mem, h264_64x48 + 30, 64); len = 64; memcpy(pic.mem + 18 * 4, &len, 4); /* slices only: nothing to hand over */
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 2 && rec.decoded == 4, "a stream without parameter sets keeps the backend's configuration");
        rec.refuse = 1; memcpy(bs.mem, h264_50x30_crop, sizeof h264_50x30_crop); len = sizeof h264_50x30_crop; memcpy(pic.mem + 18 * 4, &len, 4); int dec_before = rec.decoded; memset(st.mem, 0, 32);
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0); uint32_t stw[8]; memcpy(stw, st.mem, 32);
        CHECK(rec.configures == 3 && rec.decoded == dec_before && stw[3] == 4 && stats(d3)->errors > 0, "a refused configure stops the decode and reports error 4 (status word %u)", stw[3]);
        rec.refuse = 0;
        /* VP9: a key frame configures with a vpcC record, an inter frame does not */
        uint32_t p5[sizeof p4 / 4]; memcpy(p5, p4, sizeof p4); p5[2] = DEC_CODEC_VP9;
        memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_key_70x38, sizeof vp9_key_70x38); len = sizeof vp9_key_70x38; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 12 * 4, &len, 4);
        int c0 = rec.configures; submit(d3, &cb3, p5, sizeof p5 / 4, NULL, 0);
        CHECK(rec.configures == c0 + 1 && rec.cfg.codec == DEC_CODEC_VP9 && rec.cfg.width == 70 && rec.cfg.height == 38 && rec.cfg.vpcc_size == VP9_VPCC_SIZE && rec.cfg.vpcc[0] == 1 && rec.cfg.vpcc[5] == 10,
              "VP9 key frame: configure with %dx%d and a vpcC record", rec.cfg.width, rec.cfg.height);
        memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_inter, sizeof vp9_inter); len = sizeof vp9_inter; memcpy(pic.mem + 12 * 4, &len, 4);
        submit(d3, &cb3, p5, sizeof p5 / 4, NULL, 0);
        CHECK(rec.configures == c0 + 1, "VP9 inter frame: no configure");
        /* the SDK's way (MK8D, 2026-10-27): every register goes through the THI method FIFO (INCR 0x10 count 2: method, data) and the H.264 sequence lives in the
         * picture-info structure; the stream carries only slice NAL units, so the sets are synthesised from the structure */
        {
            memset(&rec, 0, sizeof rec);
            uint32_t pw[NVDEC_H264_PIC_WORDS] = {0};
            pw[18] = 6; pw[19] = 1; pw[22] = 1; pw[24] = 1; pw[25] = 32; pw[26] = 38; pw[28] = 1; pw[30] = 4; pw[32] = 1; pw[35] = pw[36] = 0x200;
            pw[44] = 0x16u | (1u << 12) | (0x3fu << 16) | (28u << 22) | (28u << 27); /* direct_8x8, weighted_pred, 4:2:0, qp -1, chroma offsets -4 */
            pw[45] = 2 | (1u << 2); /* weighted_bipred 2, curr_pic_idx 1 */
            memcpy(pic.mem, pw, sizeof pw);
            static const uint8_t slice[6] = {0, 0, 1, 0x65, 0x88, 0x84};
            memset(bs.mem, 0, 0x2000); memcpy(bs.mem, slice, sizeof slice);
#define THI(reg, val) INCR(NVM_THI_METHOD0, 2), (reg), (val)
            uint32_t p6[] = {SETCL(MEDIA_CLASS_NVDEC), THI(NVM_SET_APPLICATION_ID, DEC_CODEC_H264), THI(NVDEC_SET_DRV_PIC_SETUP_OFFSET, pic.iova >> 8), THI(NVDEC_SET_IN_BUF_BASE_OFFSET, bs.iova >> 8),
                             THI(NVDEC_SET_PICTURE_INDEX, 1), THI(NVDEC_SET_PICTURE_LUMA_OFFSET0 + 1, luma.iova >> 8), THI(NVDEC_SET_PICTURE_CHROMA_OFFSET0 + 1, chroma.iova >> 8),
                             THI(NVDEC_SET_STATUS_OFFSET, st.iova >> 8), THI(NVM_EXECUTE, 0)};
#undef THI
            submit(d3, &cb3, p6, sizeof p6 / 4, NULL, 0);
            H264Sps sps; H264Pps pps;
            CHECK(stats(d3)->thi_pairs == 8 && rec.configures == 1 && rec.decoded == 1 && rec.last.picture_index == 1, "THI pairs reach the registers: %llu pairs, configured %d, decoded %d", (unsigned long long)stats(d3)->thi_pairs, rec.configures, rec.decoded);
            CHECK(rec.cfg.width == 512 && rec.cfg.height == 608 && rec.cfg.h264.nsps == 1 && rec.cfg.h264.npps == 1 && !h264_parse_sps(rec.cfg.h264.sps[0] + 1, rec.cfg.h264.sps_size[0] - 1, &sps) && sps.width == 512 && sps.height == 608,
                  "H.264 without in-band sets: SPS synthesised from the picture-info structure (%dx%d)", rec.cfg.width, rec.cfg.height);
            CHECK(sps.num_ref_frames == 15 && sps.level_idc == 32, "synthesised SPS: 15 reference frames (16 leaves VideoToolbox no slot for a B picture), level %d holds 16 frames of 1216 MBs",
                  sps.level_idc);
            CHECK(!h264_parse_pps(rec.cfg.h264.pps[0] + 1, rec.cfg.h264.pps_size[0] - 1, &pps) && pps.cabac == 1 && pps.num_ref_idx_l0 == 5 && pps.weighted_pred == 1 && pps.weighted_bipred_idc == 2 && pps.pic_init_qp == 25 && pps.chroma_qp_index_offset == -4 && pps.deblocking_filter_control == 1,
                  "synthesised PPS: CABAC, 5 refs, weighted pred/bipred 2, QP 25, chroma offset %d", pps.chroma_qp_index_offset);
            submit(d3, &cb3, p6, sizeof p6 / 4, NULL, 0);
            CHECK(rec.configures == 1 && rec.decoded == 2, "the same structure again: no new configure");
            pw[25] = 20; memcpy(pic.mem, pw, sizeof pw); submit(d3, &cb3, p6, sizeof p6 / 4, NULL, 0);
            CHECK(rec.configures == 2 && rec.cfg.width == 320, "a changed picture size reconfigures (%d)", rec.cfg.width);
        }
        close_(d3);
        CHECK(stats(dec)->configures >= 1 && stats(dec)->frames > 0, "the null backend takes the configure hand-off too (%llu configures on the first channel)", (unsigned long long)stats(dec)->configures);
        /* contract checks of install */
        DecoderBackend bad = rec_backend; bad.abi = 2; CHECK(decoder_backend_install(&bad) == -1 && decoder_backend_current() == &rec_backend, "install refuses a backend with another ABI and keeps the current one");
        bad = rec_backend; bad.decode = NULL; CHECK(decoder_backend_install(&bad) == -1, "install refuses a backend without decode");
        bad = rec_backend; bad.configure = NULL; bad.reset = NULL; CHECK(decoder_backend_install(&bad) == 0, "configure and reset are optional");
        CHECK(decoder_backend_install(NULL) == 0 && decoder_backend_current() == &decoder_backend_null && decoder_backend_null.abi == DECODER_BACKEND_ABI, "NULL restores the null backend");
    }

    /* ---- VIC ---- */
    Buf vcb = mkbuf(vic, 0x1000), cfg = mkbuf(vic, 0x1000), yin = mkbuf(vic, 0x8000), cin = mkbuf(vic, 0x4000), out = mkbuf(vic, 0x10000), ochroma = mkbuf(vic, 0x8000);
    uint32_t vprog[] = {SETCL(MEDIA_CLASS_VIC), INCR(VIC_SET_CONTROL_PARAMS, 2), 0x610110, cfg.iova >> 8, INCR(VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET, 2), out.iova >> 8, ochroma.iova >> 8,
                        INCR(VIC_SET_INPUT0_LUMA_OFFSET, 2), yin.iova >> 8, cin.iova >> 8, IMM(NVM_EXECUTE, 0)};
    vic_fill(&yin, &cin, 16, 8, 256, 81, 90, 240); /* BT.601 limited red: Y 81, U 90, V 240 (input rows 256-aligned, as NVDEC writes them) */
    vic_cfg(&cfg, VIC_FMT_RGBA8, 0, 0, 16, 8);
    CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->frames == 1 && stats(vic)->errors == 0, "VIC submit (control word at 0x1c1, config at 0x1c2)");
    CHECK(out.mem[0] >= 250 && out.mem[1] <= 6 && out.mem[2] <= 6 && out.mem[3] == 255, "NV12 red -> RGBA8 bytes R,G,B,A = %u %u %u %u", out.mem[0], out.mem[1], out.mem[2], out.mem[3]);
    CHECK(out.mem[16 * 4 * 7 + 15 * 4] >= 250 && out.mem[16 * 4 * 8] == 0, "RGBA8 pitch linear: rows of width*4, last pixel written, nothing after");
    vic_cfg(&cfg, VIC_FMT_BGRA8, 0, 0, 16, 8); memset(out.mem, 0, 0x10000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[2] >= 250 && out.mem[0] <= 6 && out.mem[3] == 255, "BGRA8: red in byte 2");
    vic_fill(&yin, &cin, 16, 8, 256, 235, 128, 128); vic_cfg(&cfg, VIC_FMT_RGBX8, 0, 0, 16, 8); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[0] == 255 && out.mem[1] == 255 && out.mem[2] == 255 && out.mem[3] == 255, "limited-range white (Y 235) -> 255, RGBX alpha 255");
    vic_fill(&yin, &cin, 16, 8, 16, 16, 128, 128); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[0] == 0 && out.mem[1] == 0 && out.mem[2] == 0, "packed input rows (surface too small for 256-aligned rows) are read packed; black Y 16 -> 0");
    { /* NV12 out: luma and chroma at 256-aligned rows, pitch linear */
        for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) yin.mem[y * 256 + x] = (uint8_t)(x + 16 * y);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 16; x++) cin.mem[y * 256 + x] = (uint8_t)(200 + x);
        vic_cfg(&cfg, VIC_FMT_NV12, 0, 0, 16, 8); memset(out.mem, 0, 0x10000); memset(ochroma.mem, 0, 0x8000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
        CHECK(out.mem[0] == 0 && out.mem[15] == 15 && out.mem[256 + 3] == 19 && out.mem[256 * 7 + 15] == 127 && out.mem[16] == 0 && ochroma.mem[256 * 3 + 5] == 205,
              "NV12 output, pitch 256: luma row 1 at 256, chroma row 3 at 768");
    }
    { /* block linear: NV12 128x32, 2 GOBs per block. GOB layout: (16,0)->32 (0,1)->16 (0,2)->64 (32,0)->256 (0,8)->512; next block (64,0)->1024; next block row (0,16)->2048 */
        for (int y = 0; y < 32; y++) for (int x = 0; x < 128; x++) yin.mem[y * 256 + x] = (uint8_t)(x + 3 * y + 1);
        vic_cfg(&cfg, VIC_FMT_NV12, 1, 1, 128, 32); memset(out.mem, 0, 0x10000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
#define LY(x, y) (uint8_t)((x) + 3 * (y) + 1)
        CHECK(out.mem[0] == LY(0, 0) && out.mem[32] == LY(16, 0) && out.mem[16] == LY(0, 1) && out.mem[64] == LY(0, 2) && out.mem[256] == LY(32, 0) && out.mem[512] == LY(0, 8) &&
              out.mem[1024] == LY(64, 0) && out.mem[2048] == LY(0, 16) && out.mem[2048 + 1024 + 512 + 256 + 64 + 32 + 16 + 5] == LY(64 + 32 + 16 + 5, 16 + 8 + 2 + 1),
              "block-linear NV12 luma lands at the GOB offsets (%u %u %u %u)", out.mem[32], out.mem[16], out.mem[1024], out.mem[2048]);
#undef LY
        vic_fill(&yin, &cin, 32, 16, 256, 81, 90, 240); vic_cfg(&cfg, VIC_FMT_RGBA8, 1, 0, 32, 16); memset(out.mem, 0, 0x10000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
        CHECK(out.mem[0] >= 250 && out.mem[3] == 255 && out.mem[512 * 2 + 3] == 255 && out.mem[512 * 4 - 1] == 255 && out.mem[512 * 4] == 0,
              "block-linear RGBA8 32x16 (1 GOB high blocks): 4 GOBs written, nothing after");
    }
    errs = stats(vic)->errors;
    vic_cfg(&cfg, 0x05, 0, 0, 16, 8); CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->errors == errs + 1, "unknown output format: counted error");
    vic_cfg(&cfg, VIC_FMT_RGBA8, 0, 0, 160, 128); CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->errors == errs + 2, "output surface too small: counted error");
    { /* no readable config structure (0x1c2 not written or unmapped): the execute copies NV12 to the NV12 output surfaces */
        for (int i = 0; i < 16 * 8; i++) yin.mem[i] = (uint8_t)(i + 1);
        memset(cin.mem, 77, 16 * 4); memset(out.mem, 0, 0x100); memset(ochroma.mem, 0, 64);
        uint32_t nprog[] = {SETCL(MEDIA_CLASS_VIC), INCR(VIC_SET_CONTROL_PARAMS, 2), 0x610110, 0, INCR(VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET, 2), out.iova >> 8, ochroma.iova >> 8,
                            INCR(VIC_SET_INPUT0_LUMA_OFFSET, 2), yin.iova >> 8, cin.iova >> 8, IMM(NVM_EXECUTE, 0)};
        errs = stats(vic)->errors;
        CHECK(submit(vic, &vcb, nprog, sizeof nprog / 4, NULL, 0) == NV_OK && stats(vic)->errors == errs && stats(vic)->vic_copies == 1 && out.mem[0] == 1 && out.mem[100] == 101 && ochroma.mem[10] == 77,
              "VIC with no readable config structure: NV12 copy (%llu copies)", (unsigned long long)stats(vic)->vic_copies);
    }

    /* ---- NVJPG ---- */
    Buf jcb = mkbuf(jpg, 0x1000), jsetup = mkbuf(jpg, 0x1000), jin = mkbuf(jpg, 0x8000), jout = mkbuf(jpg, 0x8000), jchroma = mkbuf(jpg, 0x4000), jst = mkbuf(jpg, 0x100);
    int w = 40, h = 24; uint8_t *src = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *p = src + ((size_t)y * w + x) * 4; p[0] = (uint8_t)(x * 6); p[1] = (uint8_t)(y * 10); p[2] = 90; p[3] = 255; }
    size_t jn; uint8_t *jfile = napjpeg_encode_rgba(src, w, h, w * 4, 92, 0, &jn); memcpy(jin.mem, jfile, jn);
    uint32_t js[NVJPG_SETUP_WORDS] = {(uint32_t)jn, 0x8000, 0, 0, NVJPG_FMT_RGBA, 0}; memcpy(jsetup.mem, js, sizeof js);
    uint32_t jprog[] = {SETCL(MEDIA_CLASS_NVJPG), INCR(NVJPG_SET_DRV_PIC_SETUP_OFFSET, 4), jsetup.iova >> 8, jin.iova >> 8, jout.iova >> 8, jchroma.iova >> 8,
                        INCR(NVJPG_SET_STATUS_OFFSET, 1), jst.iova >> 8, IMM(NVM_EXECUTE, 0)};
    CHECK(submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0) == NV_OK && stats(jpg)->frames == 1, "NVJPG submit");
    uint32_t jstat[4]; memcpy(jstat, jst.mem, 16);
    CHECK(jstat[0] == 0 && jstat[1] == 40 && jstat[2] == 24 && jstat[3] == 40 * 24 * 4, "status: ok, 40x24, %u bytes", jstat[3]);
    double pr = psnr(src, jout.mem, (size_t)w * h * 4); CHECK(pr > 32.0, "RGBA output matches the source image (PSNR %.1f dB)", pr);
    uint32_t js2[NVJPG_SETUP_WORDS] = {(uint32_t)jn, 0x8000, 20, 12, NVJPG_FMT_RGBA, 128}; memcpy(jsetup.mem, js2, sizeof js2); memset(jout.mem, 0, 0x8000);
    submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0); memcpy(jstat, jst.mem, 16);
    CHECK(jstat[0] == 0 && jstat[1] == 20 && jstat[2] == 12 && jstat[3] == 128 * 12 && jout.mem[128 * 11 + 19 * 4 + 3] == 255, "scaled to 20x12 with pitch 128");
    uint32_t js3[NVJPG_SETUP_WORDS] = {(uint32_t)jn, 0x8000, 0, 0, NVJPG_FMT_NV12, 0}; memcpy(jsetup.mem, js3, sizeof js3);
    submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0); memcpy(jstat, jst.mem, 16);
    CHECK(jstat[0] == 0 && jstat[3] == 40 * 24 + 40 * 12, "NV12 output size %u", jstat[3]);
    { /* decode the planes back with the VIC math: Y of pixel (39,0) is bright in R only */
        uint8_t y0 = (uint8_t)(0.299f * (39 * 6) + 0.587f * 0 + 0.114f * 90 + 0.5f); CHECK(abs((int)jout.mem[39] - (int)y0) <= 6, "NV12 luma matches BT.601 of the source (%u vs %u)", jout.mem[39], y0); }
    memset(jin.mem, 0x5a, 64); memcpy(jsetup.mem, js, sizeof js); errs = stats(jpg)->errors;
    CHECK(submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0) == NV_OK && stats(jpg)->errors == errs + 1, "garbage input: counted error"); memcpy(jstat, jst.mem, 16);
    CHECK(jstat[0] == JPEGCORE_BAD, "status carries the decoder error (%u)", jstat[0]);
    memcpy(jin.mem, jfile, jn); uint32_t js4[NVJPG_SETUP_WORDS] = {(uint32_t)jn, 100, 0, 0, NVJPG_FMT_RGBA, 0}; memcpy(jsetup.mem, js4, sizeof js4);
    submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0); memcpy(jstat, jst.mem, 16); CHECK(jstat[0] != 0, "output surface too small: error in the status");
    free(src); free(jfile);

    /* ---- MSENC ---- */
    Buf ecb = mkbuf(enc, 0x1000), est = mkbuf(enc, 0x100); memset(est.mem, 0xff, 16);
    uint32_t eprog[] = {SETCL(MEDIA_CLASS_MSENC), INCR(MSENC_SET_STATUS_OFFSET, 1), est.iova >> 8, IMM(NVM_EXECUTE, 0)};
    CHECK(submit(enc, &ecb, eprog, 4, NULL, 0) == NV_OK && stats(enc)->executes == 1 && est.mem[0] == 0 && est.mem[12] == 0, "MSENC execute accepted, status reports no bitstream");

    { /* command table depth: only the undocumented command 13 stays a stub */
        int stubs = 0, rows = (int)(sizeof nv_cmds / sizeof *nv_cmds); unsigned stub_cmd = 0;
        for (int i = 0; i < rows; i++) if (nv_cmds[i].status == IPC_STUB) { stubs++; stub_cmd = nv_cmds[i].cmd; }
        CHECK(rows == 14 && stubs == 1 && stub_cmd == 13, "nvdrv command table: %d rows, %d stub (cmd %u)", rows, stubs, stub_cmd);
        memset(&call, 0, sizeof call); args[0] = 0; call.object = nv; call.in = (const uint8_t *)args; call.in_size = 8; nv_cmds[8].fn(nv, &call);
        CHECK(call.out_size == 0, "DumpGraphicsMemoryInfo replies with nothing");
        memset(&call, 0, sizeof call); call.object = nv; nv_cmds[6].fn(nv, &call);
        CHECK(call.out_size >= 0x20, "GetStatus replies with a 0x20-byte status");
    }
    /* ---- parameter sets for a VideoToolbox backend (codec_headers_core) ---- */
    {
        H264ParamSets ps; uint8_t ann[256], avcc[256], nal[128];
        CHECK(h264_find_param_sets(h264_64x48, sizeof h264_64x48, &ps) == 0 && ps.nsps == 1 && ps.npps == 1, "param sets: SPS and PPS found in an Annex B stream");
        CHECK(ps.sps[0][0] == 0x67 && ps.sps_size[0] == 22 && ps.pps[0][0] == 0x68 && ps.pps_size[0] == 5, "param sets: NAL units start at their header byte (%zu, %zu bytes)", ps.sps_size[0], ps.pps_size[0]);
        H264Sps sp; CHECK(h264_parse_sps(ps.sps[0] + 1, ps.sps_size[0] - 1, &sp) == 0 && sp.width == 64 && sp.height == 48, "param sets: the found SPS parses (%dx%d)", sp.width, sp.height);
        size_t an = h264_param_sets_annexb(&ps, ann, sizeof ann);
        H264Sps sp2; CHECK(an == 8 + ps.sps_size[0] + ps.pps_size[0] && !h264_find_sps(ann, an, &sp2) && sp2.width == 64, "param sets: Annex B form for vth264_config (%zu bytes)", an);
        CHECK(h264_param_sets_annexb(&ps, ann, 10) == 0, "param sets: too small a buffer gives 0");
        size_t ac = h264_param_sets_avcc(&ps, avcc, sizeof avcc);
        H264ParamSets back; CHECK(ac && h264_parse_avcc(avcc, ac, &back) == 0 && back.nal_length_size == 4 && back.nsps == 1 && back.npps == 1 &&
              back.sps_size[0] == ps.sps_size[0] && !memcmp(back.sps[0], ps.sps[0], ps.sps_size[0]) && !memcmp(back.pps[0], ps.pps[0], ps.pps_size[0]), "param sets: avcC round trip (%zu bytes)", ac);
        CHECK(avcc[1] == 66 && avcc[3] == 10 && avcc[4] == 0xff && avcc[5] == 0xe1, "param sets: avcC profile/level/length-size bytes");
        for (size_t cut = 0; cut < ac; cut += 3) { uint8_t t[256]; memcpy(t, avcc, cut); if (h264_parse_avcc(t, cut, &back) == 0) { CHECK(0, "truncated avcC (%zu of %zu) accepted", cut, ac); break; } }
        uint8_t bad[8] = {2, 0, 0, 0, 0xff, 0xe0, 0, 0}; CHECK(h264_parse_avcc(bad, 8, &back) == -1, "param sets: avcC with a wrong version refused");
        static const uint8_t only_sps[] = {0,0,0,1,0x67,66,192,10,218,17,236,4,64,0,0,3,0,64,0,0,3,2,131,196,137,168};
        CHECK(h264_find_param_sets(only_sps, sizeof only_sps, &ps) == 1 && ps.nsps == 1 && ps.npps == 0, "param sets: an SPS without a PPS is reported as incomplete (1)");
        CHECK(h264_find_param_sets(h265_96x64, sizeof h265_96x64, &ps) == -1, "param sets: a stream without any H.264 parameter sets: -1");
        H264Pps pp; CHECK(h264_parse_pps(h264_64x48 + 4 + 22 + 4 + 1, 4, &pp) == 0 && pp.pps_id == 0 && pp.sps_id == 0 && pp.cabac == 0 && pp.num_ref_idx_l0 == 1 && !pp.transform_8x8,
                           "PPS parse of the x264 vector: ids 0/0, CAVLC, one reference");
        /* writers: SPS and PPS built from fields come back through the parsers */
        static const struct { int w, h, refs; } sizes[] = {{64, 48, 1}, {1280, 720, 4}, {1920, 1080, 4}, {50, 30, 2}, {3840, 2160, 5}, {16, 16, 0}, {854, 480, 3}};
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            H264SpsFields f; h264_sps_fields_default(&f, sizes[i].w, sizes[i].h, sizes[i].refs);
            size_t n = h264_build_sps(&f, nal, sizeof nal); H264Sps r;
            CHECK(n > 4 && nal[0] == 0x67 && !h264_parse_sps(nal + 1, n - 1, &r) && r.width == sizes[i].w && r.height == sizes[i].h && r.num_ref_frames == sizes[i].refs &&
                  r.profile_idc == 100 && r.chroma_format_idc == 1 && r.bit_depth == 8 && r.frame_mbs_only == 1 && r.poc_type == 2,
                  "built SPS %dx%d parses back (%zu bytes, %dx%d, level %d)", sizes[i].w, sizes[i].h, n, r.width, r.height, r.level_idc);
        }
        { H264SpsFields f; h264_sps_fields_default(&f, 1280, 720, 4); f.profile_idc = 66; f.poc_type = 0; f.log2_max_poc_lsb = 8; f.frame_mbs_only = 0; f.height_map_units = 23; f.mb_adaptive_frame_field = 1; f.crop_bottom = 0;
          size_t n = h264_build_sps(&f, nal, sizeof nal); H264Sps r;
          CHECK(n && !h264_parse_sps(nal + 1, n - 1, &r) && r.profile_idc == 66 && r.poc_type == 0 && r.frame_mbs_only == 0 && r.width == 1280 && r.height == 2 * 23 * 16 && r.chroma_format_idc == 1,
                "built SPS: Baseline profile, poc type 0, interlaced (%dx%d)", r.width, r.height);
          f.poc_type = 1; CHECK(h264_build_sps(&f, nal, sizeof nal) == 0, "built SPS: poc type 1 is not writable"); f.poc_type = 2; f.width_mbs = 0; CHECK(h264_build_sps(&f, nal, sizeof nal) == 0, "built SPS: zero width refused");
          h264_sps_fields_default(&f, 640, 480, 2); CHECK(h264_build_sps(&f, nal, 5) == 0, "built SPS: too small an output buffer gives 0"); }
        { H264SpsFields f; h264_sps_fields_default(&f, 1920, 1080, 4); f.profile_idc = 110; f.bit_depth = 10; f.chroma_format_idc = 2; f.crop_bottom = 8; /* 4:2:2 crop unit is one row */ size_t n = h264_build_sps(&f, nal, sizeof nal); H264Sps r;
          CHECK(n && !h264_parse_sps(nal + 1, n - 1, &r) && r.bit_depth == 10 && r.chroma_format_idc == 2 && r.width == 1920 && r.height == 1080, "built SPS: High 4:2:2 10-bit (%dx%d, depth %d)", r.width, r.height, r.bit_depth); }
        for (int ext = 0; ext < 2; ext++) {
            H264Pps in = {.pps_id = 3, .sps_id = 1, .cabac = 1, .bottom_field_pic_order = 0, .num_slice_groups = 1, .num_ref_idx_l0 = 4, .num_ref_idx_l1 = 2, .weighted_pred = 1, .weighted_bipred_idc = 2,
                          .pic_init_qp = 20, .pic_init_qs = 30, .chroma_qp_index_offset = -3, .deblocking_filter_control = 1, .constrained_intra_pred = 0, .redundant_pic_cnt = 0,
                          .transform_8x8 = ext, .second_chroma_qp_index_offset = ext ? 5 : -3};
            size_t n = h264_build_pps(&in, nal, sizeof nal); H264Pps out;
            CHECK(n && nal[0] == 0x68 && !h264_parse_pps(nal + 1, n - 1, &out) && out.pps_id == 3 && out.sps_id == 1 && out.cabac == 1 && out.num_ref_idx_l0 == 4 && out.num_ref_idx_l1 == 2 &&
                  out.weighted_pred == 1 && out.weighted_bipred_idc == 2 && out.pic_init_qp == 20 && out.pic_init_qs == 30 && out.chroma_qp_index_offset == -3 && out.deblocking_filter_control == 1 &&
                  out.transform_8x8 == ext && out.second_chroma_qp_index_offset == (ext ? 5 : -3), "built PPS (extension %d) parses back (%zu bytes)", ext, n);
        }
        { H264Pps bad_pps = {.num_slice_groups = 1, .num_ref_idx_l0 = 0, .num_ref_idx_l1 = 1}; CHECK(h264_build_pps(&bad_pps, nal, sizeof nal) == 0, "built PPS: zero reference count refused"); }
        { /* a built SPS + PPS pair makes a stream the null backend and the finder both accept */
            H264SpsFields f; h264_sps_fields_default(&f, 320, 240, 2); uint8_t st[128]; size_t at = 0;
            st[at++] = 0; st[at++] = 0; st[at++] = 0; st[at++] = 1; at += h264_build_sps(&f, st + at, sizeof st - at);
            H264Pps p = {.num_slice_groups = 1, .num_ref_idx_l0 = 1, .num_ref_idx_l1 = 1, .pic_init_qp = 26, .pic_init_qs = 26};
            st[at++] = 0; st[at++] = 0; st[at++] = 0; st[at++] = 1; at += h264_build_pps(&p, st + at, sizeof st - at);
            H264Sps r; CHECK(h264_find_param_sets(st, at, &ps) == 0 && !h264_find_sps(st, at, &r) && r.width == 320 && r.height == 240, "built SPS + PPS in one Annex B stream: found and parsed (%zu bytes)", at);
        }
        /* VP9: vpcC from a key frame */
        Vp9Header vh; uint8_t vpcc[VP9_VPCC_SIZE];
        CHECK(vp9_parse_header(vp9_key_70x38, sizeof vp9_key_70x38, &vh) == 0 && vh.key_frame && vh.width == 70 && vh.height == 38 && vh.subsampling_x == 1 && vh.subsampling_y == 1, "VP9 key frame: 4:2:0");
        CHECK(vp9_build_vpcc(&vh, vpcc) == VP9_VPCC_SIZE && vpcc[0] == 1 && vpcc[1] == 0 && vpcc[4] == 0 && vpcc[5] == 10 && (vpcc[6] >> 4) == 8 && ((vpcc[6] >> 1) & 7) == 1 && !vpcc[10] && !vpcc[11],
              "vpcC: version 1, profile 0, level 1.0, 8-bit 4:2:0 (%02x %02x %02x)", vpcc[4], vpcc[5], vpcc[6]);
        Vp9Header inter; vp9_parse_header(vp9_inter, sizeof vp9_inter, &inter);
        CHECK(vp9_build_vpcc(&inter, vpcc) == 0, "vpcC: an inter frame has no size, no record");
        CHECK(vp9_level_for(1280, 720) == 31 && vp9_level_for(1920, 1080) == 40 && vp9_level_for(3840, 2160) == 50 && vp9_level_for(64, 64) == 10, "VP9 levels by picture size");
    }

    /* ---- /dev/nvdisp-disp0: display, vblank and syncpoint queries (proposed ABI, see nvdrv_media_core.h) ---- */
    {
        NvDisp d; uint8_t b[32]; uint64_t T = 5000000000ull;
        nvdisp_init(&d, T, 0);
        memset(b, 0xee, sizeof b);
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_MODE, b, 32, T) == NVDISP_OK, "disp core: GET_MODE");
        uint32_t m[8]; memcpy(m, b, 32);
        CHECK(m[0] == 1280 && m[1] == 720 && m[2] == 60000 && m[3] == 32 && m[4] == 0 && m[5] == 0, "disp core: handheld 1280x720 at 60 Hz, not docked");
        nvdisp_init(&d, T, 1); nvdisp_ioctl(&d, NVDISP_GET_MODE, b, 32, T); memcpy(m, b, 32);
        CHECK(m[0] == 1920 && m[1] == 1080 && m[4] == 1, "disp core: docked 1920x1080");
        CHECK(nvdisp_vblank_count(&d, T) == 0 && nvdisp_vblank_count(&d, T + 1000000000ull) == 60 && nvdisp_vblank_count(&d, T + 500000000ull) == 30, "disp core: 60 vblanks per second");
        CHECK(nvdisp_vblank_count(&d, T - 1) == 0, "disp core: a clock before the start reads 0");
        uint64_t c, ts; nvdisp_ioctl(&d, NVDISP_GET_VBLANK, b, 16, T + 100000000ull); memcpy(&c, b, 8); memcpy(&ts, b + 8, 8);
        CHECK(c == 6 && ts == T + 100000000ull, "disp core: vblank 6 at +100 ms (count %llu ts %llu)", (unsigned long long)c, (unsigned long long)(ts - T));
        uint8_t w[24] = {0}; uint64_t n1, n2, t2;
        nvdisp_ioctl(&d, NVDISP_WAIT_VBLANK, w, 24, T + 100000000ull); memcpy(&n1, w + 8, 8);
        nvdisp_ioctl(&d, NVDISP_WAIT_VBLANK, w, 24, T + 100000000ull); memcpy(&n2, w + 8, 8); memcpy(&t2, w + 16, 8);
        CHECK(n1 == 7 && n2 == 8 && t2 > T + 100000000ull, "disp core: WAIT_VBLANK reports the next vblank and never repeats one (%llu then %llu)", (unsigned long long)n1, (unsigned long long)n2);
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_MODE, b, 8, T) == NVDISP_BAD_PARAMETER && nvdisp_ioctl(&d, NVDISP_WAIT_VBLANK, w, 16, T) == NVDISP_BAD_PARAMETER, "disp core: short buffers refused");
        CHECK(nvdisp_ioctl(&d, 0x77, b, 32, T) == NVDISP_NOT_IMPLEMENTED, "disp core: unknown nr is NotImplemented");
        nvdisp_ioctl(&d, NVDISP_GET_STATUS, b, 16, T); memcpy(m, b, 16);
        CHECK(m[0] == 1 && m[1] == 1 && m[2] == 1 && m[3] == 1, "disp core: connected, powered, docked, vblank enabled");
    }
    {
        uint32_t d0 = open_("/dev/nvdisp-disp0"), d1 = open_("/dev/nvdisp-disp1");
        CHECK(d0 && d1 && d0 != d1 && fds[d0].kind == DEV_DISP && fds[d1].kind == DEV_DISP, "nvdisp-disp0/1 open as display devices (fds %u %u)", d0, d1);
        uint32_t mode[8] = {0};
        uint32_t e = ioctl_(d0, cmd_id(2, 32, NVDISP_IOCTL_TYPE, NVDISP_GET_MODE), mode, 32);
        CHECK(e == 0 && mode[0] == 1280 && mode[1] == 720, "nvdisp-disp0 GET_MODE through the ioctl handler: %ux%u", mode[0], mode[1]);
        uint32_t sp[2] = {0};
        e = ioctl_(d0, cmd_id(2, 8, NVDISP_IOCTL_TYPE, NVDISP_GET_SYNCPT), sp, 8); uint32_t sp1[2] = {0};
        ioctl_(d1, cmd_id(2, 8, NVDISP_IOCTL_TYPE, NVDISP_GET_SYNCPT), sp1, 8);
        CHECK(e == 0 && sp[0] == 26 && sp1[0] == 27, "nvdisp vblank syncpoints 26 and 27 (got %u %u)", sp[0], sp1[0]);
        uint64_t v1[3] = {0}, v2[3] = {0};
        ioctl_(d0, cmd_id(3, 24, NVDISP_IOCTL_TYPE, NVDISP_WAIT_VBLANK), v1, 24); ioctl_(d0, cmd_id(3, 24, NVDISP_IOCTL_TYPE, NVDISP_WAIT_VBLANK), v2, 24);
        CHECK(v1[1] && v2[1] == v1[1] + 1, "nvdisp WAIT_VBLANK counts up through the handler (%llu, %llu)", (unsigned long long)v1[1], (unsigned long long)v2[1]);
        uint32_t junk[4] = {0};
        CHECK(ioctl_(d0, cmd_id(3, 16, 'X', 1), junk, 16) == NV_NOT_IMPLEMENTED, "nvdisp: another ioctl type is NotImplemented");
        CHECK(ioctl_(d0, cmd_id(3, 8, NVDISP_IOCTL_TYPE, NVDISP_GET_MODE), junk, 8) == NV_BAD_PARAMETER, "nvdisp: a too-small struct is BadParameter");
        close_(d0); close_(d1);
    }

    /* close frees the engines; a submit on a channel without addresses still behaves as before */
    close_(dec); close_(vic); close_(jpg); close_(enc);
    CHECK(fds[dec].ch.eng == NULL, "engine released with the channel");
    for (int i = 0; i < 5; i++) {} /* (buffers are intentionally leaked: test process) */
    printf("{\"test\":\"svc_nvdrv_media\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
