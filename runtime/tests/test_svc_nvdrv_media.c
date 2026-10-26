/* Unit tests: nvdrv multimedia engines (nvdrv_media_core behind nvdrv.c): host1x command streams submitted through the real ioctl handlers,
 * NVDEC with the null backend and a recording backend, VIC colour conversion, NVJPG decode (synthetic JPEGs), MSENC, relocations, gathers,
 * malformed streams, unmapped surfaces. Register numbers come from nvdrv_media_regs.h (unverified against real traces). */
#include "../src/services/nvdrv.c"
#include "../src/services/nvdrv_media_regs.h"
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

/* A backend that records what it is asked and produces a 16x16 frame. */
static struct { int opened, closed, decoded, codec; DecodeRequest last; uint8_t pic_head[8]; } rec;
static void *rec_open(int codec) { rec.opened++; rec.codec = codec; return &rec; }
static void rec_close(void *c) { (void)c; rec.closed++; }
static int rec_decode(void *c, const DecodeRequest *r, DecodeResult *o) {
    (void)c; rec.decoded++; rec.last = *r; if (r->pic) memcpy(rec.pic_head, r->pic, 8);
    memset(r->luma, 0x55, 16 * 16); memset(r->chroma, 0x66, 16 * 8); *o = (DecodeResult){16, 16, 16, 0, 0}; return 0;
}
static const DecoderBackend rec_backend = {"recording", rec_open, rec_close, rec_decode, NULL};

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
    memset(luma.mem, 0, 0x4000);
    submit(dec, &cb, slot, sizeof slot / 4, NULL, 0);
    CHECK(luma.mem[0] == 0 && luma.mem[0x1000] != 0 && chroma.mem[0x1000] == 128, "picture index 2 writes the slot-2 surfaces");

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

    /* ---- VIC ---- */
    Buf vcb = mkbuf(vic, 0x1000), cfg = mkbuf(vic, 0x1000), yin = mkbuf(vic, 0x1000), cin = mkbuf(vic, 0x1000), out = mkbuf(vic, 0x10000);
    memset(yin.mem, 81, 16 * 8); memset(cin.mem, 90, 16 * 4); for (int i = 0; i < 16 * 4; i += 2) cin.mem[i + 1] = 240; /* BT.601 limited red: Y 81, U 90, V 240 */
    uint32_t c1[VIC_CFG_WORDS] = {16, 8, 0, 0, VIC_FMT_ABGR, 0, 0, 200, 0}; memcpy(cfg.mem, c1, sizeof c1);
    uint32_t vprog[] = {SETCL(MEDIA_CLASS_VIC), INCR(VIC_SET_CONFIG_STRUCT_OFFSET, 2), cfg.iova >> 8, out.iova >> 8, INCR(VIC_SET_INPUT0_LUMA_OFFSET, 2), yin.iova >> 8, cin.iova >> 8, IMM(NVM_EXECUTE, 0)};
    CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->frames == 1 && stats(vic)->errors == 0, "VIC submit");
    CHECK(out.mem[0] >= 250 && out.mem[1] <= 6 && out.mem[2] <= 6 && out.mem[3] == 200, "NV12 red -> RGBA bytes R,G,B,A = %u %u %u %u", out.mem[0], out.mem[1], out.mem[2], out.mem[3]);
    uint32_t c2[VIC_CFG_WORDS] = {16, 8, 32, 16, VIC_FMT_ARGB, 0, 0, 255, 160}; memcpy(cfg.mem, c2, sizeof c2); memset(out.mem, 0, 0x10000);
    submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[2] >= 250 && out.mem[0] <= 6 && out.mem[3] == 255 && out.mem[160 * 15 + 32 * 4 - 2] >= 250, "ARGB (B,G,R,A) at 32x16 with pitch 160: red in byte 2, last row written");
    uint32_t c3[VIC_CFG_WORDS] = {16, 8, 0, 0, VIC_FMT_XBGR, 1, 1, 0, 0}; memcpy(cfg.mem, c3, sizeof c3);
    memset(yin.mem, 255, 16 * 8); memset(cin.mem, 128, 16 * 4); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[0] == 255 && out.mem[1] == 255 && out.mem[2] == 255 && out.mem[3] == 255, "full-range white, X format has alpha 255");
    memset(yin.mem, 16, 16 * 8); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0); CHECK(out.mem[0] == 16, "full range does not remove the black offset (Y 16 -> 16)");
    uint32_t c4[VIC_CFG_WORDS] = {0}; memcpy(cfg.mem, c4, sizeof c4); errs = stats(vic)->errors;
    CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->errors == errs + 1, "empty VIC configuration: counted error");

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

    /* close frees the engines; a submit on a channel without addresses still behaves as before */
    close_(dec); close_(vic); close_(jpg); close_(enc);
    CHECK(fds[dec].ch.eng == NULL, "engine released with the channel");
    for (int i = 0; i < 5; i++) {} /* (buffers are intentionally leaked: test process) */
    printf("{\"test\":\"svc_nvdrv_media\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
