/* Unit tests: the IR camera simulator on synthetic frames (moment, clustering, image transfer, webcam adapter, access flow, the irs service over a source). */
#include "../src/services/irs.c"
#include "svc_test.h"
#include <unistd.h>
#include <math.h>

static uint8_t F[IR_W * IR_H];
static void fill(uint8_t v) { memset(F, v, sizeof F); }
static void rect(int x0, int y0, int w, int h, uint8_t v) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) F[y * IR_W + x] = v;
}
static int near(double a, double b) { return fabs(a - b) < 1e-3; }
static int wait_sampling(const uint8_t *shm, unsigned cam, int cluster, uint64_t min) {
    for (int i = 0; i < 100; i++) {
        const IrClusterState *s = (const IrClusterState *)(shm + cam * IR_SLOT_SIZE + IR_SLOT_CLUSTER);
        const IrMomentState *m = (const IrMomentState *)(shm + cam * IR_SLOT_SIZE + IR_SLOT_MOMENT);
        uint64_t n = cluster ? s->sampling_number : m->sampling_number;
        if (n >= min) return 1;
        usleep(20000);
    }
    return 0;
}
/* access source whose answer the test controls */
static int acc_answer, acc_calls, acc_started, acc_stopped;
static void (*acc_done)(void *, int);
static void *acc_cb;
static int acc_req(void *ctx, void (*done)(void *, int), void *cb) { (void)ctx; acc_calls++; acc_done = done; acc_cb = cb; return acc_answer; }
static int acc_start(void *ctx) { (void)ctx; acc_started++; return 0; }
static void acc_stop(void *ctx) { (void)ctx; acc_stopped++; }
static int acc_frame(void *ctx, uint8_t *out, uint64_t *ts) { (void)ctx; memset(out, 200, IR_W * IR_H); *ts = 1; return 1; }

int main(void) {
    /* ---- moment ---- */
    IrMomentConfig mc; IrMomentData md[IR_MOMENT_BLOCKS]; memset(&mc, 0, sizeof mc);
    fill(100); ir_moment_compute(F, &mc, md);
    CHECK(near(md[0].avg_intensity, 100) && near(md[47].avg_intensity, 100), "flat frame: every block averages the value");
    CHECK(near(md[0].cx, 19.5) && near(md[0].cy, 19.5) && near(md[7].cx, 299.5), "block centroid of a flat block is its centre (40x40 blocks)");
    fill(0); rect(50, 10, 4, 4, 255); ir_moment_compute(F, &mc, md); /* block (1,0): x 40..79, y 0..39 */
    CHECK(near(md[1].avg_intensity, 255.0 * 16 / 1600) && near(md[1].cx, 51.5) && near(md[1].cy, 11.5), "a bright square: weighted centroid inside its block");
    CHECK(near(md[0].avg_intensity, 0) && near(md[0].cx, 19.5), "an empty block reports 0 and its centre");
    mc.window = (IrRect){0, 0, 160, 120}; ir_moment_compute(F, &mc, md);
    CHECK(near(md[2].avg_intensity, 255.0 * 16 / (20 * 20)) && near(md[2].cx, 51.5), "window of interest changes the block size (20x20)");
    mc.window = (IrRect){0, 0, 0, 0}; mc.cam.negative = 1; fill(10); ir_moment_compute(F, &mc, md);
    CHECK(near(md[5].avg_intensity, 245), "negative image inverts");
    mc.cam.negative = 0; mc.preprocess = IR_PRE_BINARIZE; mc.threshold = 50; fill(60); ir_moment_compute(F, &mc, md); CHECK(near(md[3].avg_intensity, 255), "binarize: at or above the threshold becomes 255");
    fill(40); ir_moment_compute(F, &mc, md); CHECK(near(md[3].avg_intensity, 0), "binarize: below becomes 0");
    mc.preprocess = IR_PRE_CUTOFF; fill(40); ir_moment_compute(F, &mc, md); CHECK(near(md[3].avg_intensity, 0), "cutoff drops values under the threshold");
    fill(80); ir_moment_compute(F, &mc, md); CHECK(near(md[3].avg_intensity, 80), "cutoff keeps values over it");
    mc.window = (IrRect){300, 200, 100, 100}; mc.preprocess = 0; fill(9); ir_moment_compute(F, &mc, md); CHECK(near(md[0].avg_intensity, 9), "a window past the frame edge is clamped");

    /* ---- clustering ---- */
    IrClusteringConfig kc; IrClusterData cd[IR_MAX_CLUSTERS]; memset(&kc, 0, sizeof kc); kc.intensity_min = 128;
    fill(0); rect(10, 10, 5, 4, 200); rect(100, 100, 8, 8, 255); rect(200, 50, 2, 2, 255);
    int n = ir_cluster_compute(F, &kc, cd);
    CHECK(n == 3 && cd[0].pixel_count == 64 && cd[1].pixel_count == 20 && cd[2].pixel_count == 4, "three clusters, largest first (64, 20, 4 pixels)");
    CHECK(cd[0].bx == 100 && cd[0].by == 100 && cd[0].bw == 8 && cd[0].bh == 8 && near(cd[0].cx, 103.5) && near(cd[0].cy, 103.5) && cd[0].avg_intensity == 255, "cluster bounds, centroid and average");
    CHECK(cd[1].avg_intensity == 200 && near(cd[1].cx, 12) && near(cd[1].cy, 11.5), "second cluster average and centroid");
    kc.pixel_min = 10; n = ir_cluster_compute(F, &kc, cd); CHECK(n == 2, "pixel_min drops the 4-pixel cluster");
    kc.pixel_max = 30; n = ir_cluster_compute(F, &kc, cd); CHECK(n == 1 && cd[0].pixel_count == 20, "pixel_max drops the 64-pixel cluster");
    kc.pixel_min = kc.pixel_max = 0; kc.intensity_min = 220; n = ir_cluster_compute(F, &kc, cd); CHECK(n == 2 && cd[0].pixel_count == 64, "intensity_min drops the dimmer cluster");
    fill(0); F[5 * IR_W + 5] = F[6 * IR_W + 6] = F[7 * IR_W + 5] = 255; kc.intensity_min = 128; n = ir_cluster_compute(F, &kc, cd); CHECK(n == 1 && cd[0].pixel_count == 3, "diagonal neighbours are one cluster (8-connected)");
    fill(0); rect(10, 10, 20, 20, 255); kc.window = (IrRect){15, 15, 100, 100}; n = ir_cluster_compute(F, &kc, cd); CHECK(n == 1 && cd[0].pixel_count == 15 * 15 && cd[0].bx == 15, "the window crops the cluster");
    kc.window = (IrRect){0, 0, 0, 0}; fill(0);
    for (int i = 0; i < 20; i++) rect(8 * i + 1, 8, 3, 3, 255); /* 20 separate 9-pixel clusters */
    n = ir_cluster_compute(F, &kc, cd); CHECK(n == IR_MAX_CLUSTERS, "at most 16 clusters are reported");
    fill(255); n = ir_cluster_compute(F, &kc, cd); CHECK(n == 1 && cd[0].pixel_count == IR_W * IR_H, "a full frame is one cluster (no stack overflow)");
    kc.cam.negative = 1; fill(0); rect(1, 1, 2, 2, 255); n = ir_cluster_compute(F, &kc, cd); CHECK(n == 1 && cd[0].pixel_count == IR_W * IR_H - 4, "negative image clusters the dark pixels");

    /* ---- image transfer ---- */
    IrTransferConfig tc; static uint8_t img[IR_W * IR_H]; int w, h; memset(&tc, 0, sizeof tc);
    for (int y = 0; y < IR_H; y++) for (int x = 0; x < IR_W; x++) F[y * IR_W + x] = (uint8_t)(x & 0xff);
    size_t len = ir_image_make(F, &tc, img, &w, &h); CHECK(len == IR_W * IR_H && w == 320 && h == 240 && img[7] == 7, "320x240 is the frame itself");
    tc.origin_format = tc.trimming_format = IR_FMT_160x120; len = ir_image_make(F, &tc, img, &w, &h);
    CHECK(len == 160 * 120 && w == 160 && h == 120 && img[0] == (0 + 1 + 1) / 2 + 0 && img[3] == (6 + 7 + 1) / 2, "160x120 averages 2x2 blocks");
    tc.origin_format = IR_FMT_20x15; tc.trimming_format = IR_FMT_20x15; len = ir_image_make(F, &tc, img, &w, &h); CHECK(len == 300 && img[1] == 24, "20x15: 16x16 box average of x 16..31 is 23.5, rounded up to 24");
    tc.origin_format = IR_FMT_80x60; tc.trimming_format = IR_FMT_40x30; tc.trim_x = 10; tc.trim_y = 5; len = ir_image_make(F, &tc, img, &w, &h);
    CHECK(len == 40 * 30 && w == 40 && h == 30 && img[0] == (uint8_t)((40 + 41 + 42 + 43 + 2) / 4), "trimming takes a 40x30 window from the 80x60 image at (10,5)");
    tc.trim_x = 70; tc.trim_y = 60; ir_image_make(F, &tc, img, &w, &h); CHECK(w == 40 && h == 30, "a trim past the edge is clamped to fit");
    tc.origin_format = IR_FMT_40x30; tc.trimming_format = IR_FMT_320x240; ir_image_make(F, &tc, img, &w, &h); CHECK(w == 40 && h == 30, "trim larger than the origin is clamped");
    tc.origin_format = 99; tc.trimming_format = 99; len = ir_image_make(F, &tc, img, &w, &h); CHECK(len == IR_W * IR_H, "unknown format falls back to 320x240");
    fill(30); CHECK(ir_ambient_noise(F) == 0, "dark: low noise"); fill(70); CHECK(ir_ambient_noise(F) == 1, "medium"); fill(200); CHECK(ir_ambient_noise(F) == 2, "bright: high");

    /* ---- webcam adapter ---- */
    static uint8_t rgb[640 * 480 * 4]; static uint8_t ir[IR_W * IR_H];
    IrAdapterOpts ao = {0, 0, 0, 0};
    for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) { uint8_t *p = rgb + (y * 640 + x) * 4; p[0] = p[1] = p[2] = (uint8_t)(x * 255 / 639); p[3] = 255; }
    CHECK(irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 4, 4, 0, &ao, ir) == 0, "adapter accepts RGBA");
    CHECK(ir[0] < 3 && ir[IR_W - 1] > 252 && ir[160] > 120 && ir[160] < 135, "luminance gradient is resampled to 320x240");
    ao.mirror = 1; irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 4, 4, 0, &ao, ir); CHECK(ir[0] > 252 && ir[IR_W - 1] < 3, "mirror flips horizontally");
    memset(rgb, 0, sizeof rgb); for (int y = 100; y < 380; y++) for (int x = 200; x < 440; x++) { uint8_t *p = rgb + (y * 640 + x) * 3; p[0] = 255; p[1] = 0; p[2] = 0; }
    ao = (IrAdapterOpts){0, 0, 0, 0}; irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 3, 3, 0, &ao, ir); CHECK(ir[120 * IR_W + 160] == 76, "red luminance is 0.299*255 (RGB order)");
    irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 3, 3, 1, &ao, ir); CHECK(ir[120 * IR_W + 160] == 29, "the same bytes as BGR give the blue weight (0.114*255)");
    memset(rgb, 0, sizeof rgb); for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) rgb[(y * 640 + x) * 3] = rgb[(y * 640 + x) * 3 + 1] = rgb[(y * 640 + x) * 3 + 2] = (uint8_t)(100 + (x < 320 ? 0 : 20));
    ao = (IrAdapterOpts){0, 1, 0, 0}; irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 3, 3, 0, &ao, ir); CHECK(ir[0] == 0 && ir[IR_W - 1] == 255, "contrast stretch maps the dim range to 0..255");
    memset(rgb, 90, sizeof rgb); irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 3, 3, 0, &ao, ir); CHECK(ir[0] == 90, "a flat image is not stretched");
    ao = (IrAdapterOpts){0, 0, 100, 1}; for (int i = 0; i < 640 * 480; i++) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (uint8_t)(i % 640 < 320 ? 50 : 150);
    irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 3, 3, 0, &ao, ir); CHECK(ir[0] == 0 && ir[IR_W - 1] == 255, "threshold + binarize");
    ao = (IrAdapterOpts){0, 0, 100, 0}; irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 3, 3, 0, &ao, ir); CHECK(ir[0] == 0 && ir[IR_W - 1] == 150, "threshold alone is a cutoff");
    CHECK(irs_adapter_rgb_to_ir(rgb, 640, 480, 100, 3, 0, &ao, ir) == -1 && irs_adapter_rgb_to_ir(NULL, 640, 480, 640, 3, 0, &ao, ir) == -1 && irs_adapter_rgb_to_ir(rgb, 640, 480, 640 * 3, 2, 0, &ao, ir) == -1, "bad stride, NULL source and bad channel count rejected");
    { static uint8_t wide[1280 * 360 * 3]; memset(wide, 0, sizeof wide); for (int y = 0; y < 360; y++) for (int x = 320; x < 960; x++) wide[(y * 1280 + x) * 3] = wide[(y * 1280 + x) * 3 + 1] = wide[(y * 1280 + x) * 3 + 2] = 200;
      ao = (IrAdapterOpts){0, 0, 0, 0}; irs_adapter_rgb_to_ir(wide, 1280, 360, 1280 * 3, 3, 0, &ao, ir); CHECK(ir[0] == 200 && ir[IR_W - 1] == 200, "a 32:9 frame is cropped to its central 4:3"); }

    /* ---- scripted sources ---- */
    IrFrameSource ss; uint64_t ts;
    CHECK(irs_source_from_spec("blob:100,80,10,2,0", &ss) == 0 && ss.get_frame(ss.ctx, F, &ts) == 1 && F[80 * IR_W + 100] == 255 && F[80 * IR_W + 90] == 255 && F[80 * IR_W + 89] == 0, "blob source draws a disc");
    ss.get_frame(ss.ctx, F, &ts); CHECK(F[80 * IR_W + 102] == 255 && F[80 * IR_W + 111] == 255 && F[80 * IR_W + 91] == 0, "and moves by dx per frame");
    CHECK(irs_source_from_spec("flat:77", &ss) == 0 && (ss.get_frame(ss.ctx, F, &ts), F[1234] == 77), "flat source"); CHECK(irs_source_from_spec("dark", &ss) == 0 && (ss.get_frame(ss.ctx, F, &ts), F[1234] == 0), "dark source");
    CHECK(irs_source_from_spec("nonsense", &ss) == -1 && irs_source_from_spec("", &ss) == -1 && irs_source_from_spec(NULL, &ss) == -1, "bad spec rejected");

    /* ---- core: access flow ---- */
    IrsCore *kc2 = calloc(1, sizeof *kc2); irs_core_init(kc2);
    CHECK(irs_core_run(kc2, 0, IR_MODE_MOMENT, NULL, NULL, 0) == HZ_R_IRSENSOR_NOT_CONNECTED, "no source: not connected");
    IrFrameSource as = {.ctx = NULL, .name = "access", .request_access = acc_req, .start = acc_start, .stop = acc_stop, .get_frame = acc_frame};
    irs_core_set_source(kc2, &as); acc_answer = IR_ACCESS_DENIED; acc_calls = 0;
    CHECK(acc_calls == 0, "installing a source does not ask for access");
    CHECK(irs_core_run(kc2, 0, IR_MODE_MOMENT, NULL, NULL, 0) == HZ_R_IRSENSOR_NOT_CONNECTED && acc_calls == 1 && !acc_started, "starting a processor asks; denied: not connected, the camera is never started");
    CHECK(irs_core_run(kc2, 0, IR_MODE_MOMENT, NULL, NULL, 0) == HZ_R_IRSENSOR_NOT_CONNECTED && acc_calls == 1, "a denial is remembered (no second prompt)");
    irs_core_set_source(kc2, &as); acc_answer = IR_ACCESS_PENDING; acc_calls = 0; acc_started = 0;
    CHECK(irs_core_run(kc2, 1, IR_MODE_CLUSTERING, &(IrClusteringConfig){.intensity_min = 128}, NULL, 0) == 0 && acc_calls == 1 && acc_started == 1, "pending: the processor starts");
    _Alignas(4096) static uint8_t shm2[IR_SHM_SIZE]; /* page aligned like ipc_shared_memory_new (arm64 atomics need 8-byte alignment) */ memset(shm2, 0, sizeof shm2);
    CHECK(irs_core_pump(kc2, shm2, 5) == 1, "pending access: processors run on dark frames");
    { const IrClusterState *cs = (const IrClusterState *)(shm2 + 1 * IR_SLOT_SIZE + IR_SLOT_CLUSTER); CHECK(cs->count == 0 && cs->sampling_number == 1 && (cs->seq & 1) == 0, "dark frame: no clusters, sampling number 1, seq even"); }
    acc_done(acc_cb, 1); CHECK(irs_core_pump(kc2, shm2, 6) == 1, "after the grant the real frames flow");
    { const IrClusterState *cs = (const IrClusterState *)(shm2 + 1 * IR_SLOT_SIZE + IR_SLOT_CLUSTER); CHECK(cs->count == 0 || cs->count == 1, "state updated"); CHECK(cs->sampling_number == 2, "sampling number counts frames"); }
    CHECK(irs_core_run(kc2, 99, IR_MODE_MOMENT, NULL, NULL, 0) == HZ_R_IRSENSOR_NOT_CONNECTED, "unknown camera id");
    irs_core_stop(kc2, 1); CHECK(irs_core_running(kc2) == 0 && acc_stopped == 1, "stopping the last processor stops the camera");
    CHECK(irs_core_pump(kc2, shm2, 7) == 0, "nothing runs: pump is a no-op");
    CHECK(irs_core_image(kc2, 0, NULL, 0, &(IrTransferState){0}) == -1, "image from a camera that is not transferring: -1");
    irs_core_free(kc2); free(kc2);

    /* ---- the service over a scripted source ---- */
    IpcObject *o = irs_open(); IpcCall c; uint32_t id = 0;
    svc_call(o, 304, NULL, 0, &c); IpcSharedMemory *m = handle_get(c.out_copy[0], OBJ_SHMEM); const uint8_t *shm_host = m->host;
    svc_call(o, 306, NULL, 0, &c); CHECK(c.result == HZ_R_IRSENSOR_NOT_CONNECTED, "service: no source yet -> not connected");
    svc_call(o, 314, NULL, 0, &c); CHECK(c.result == HZ_R_IRSENSOR_NOT_CONNECTED, "CheckFirmwareVersion without a source");
    env_checked = 0; /* the environment is read at the first use; the test re-arms it */
    setenv("NAPPLE_IR_SOURCE", "blob:100,80,10", 1);
    { uint8_t in[16 + sizeof(IrClusteringConfig)] = {0}; in[0] = 0; IrClusteringConfig cfg; memset(&cfg, 0, sizeof cfg); cfg.intensity_min = 128; memcpy(in + 16, &cfg, sizeof cfg);
      svc_call(o, 307, in, sizeof in, &c); CHECK(c.result == 0, "RunClusteringProcessor starts with NAPPLE_IR_SOURCE set");
      CHECK(wait_sampling(shm_host, 0, 1, 3), "the pump thread produces cluster states in the shared memory");
      const IrClusterState *cs = (const IrClusterState *)(shm_host + IR_SLOT_CLUSTER);
      IrClusterState snap; do { snap = *cs; } while ((snap.seq & 1) || snap.seq != cs->seq);
      CHECK(snap.count == 1 && snap.data[0].pixel_count > 300 && snap.data[0].pixel_count < 330 && near(snap.data[0].cx, 100) && near(snap.data[0].cy, 80) && snap.data[0].bw == 21, "the disc of radius 10 at (100,80) is found (area ~314, bounds 21)");
      uint32_t mode; memcpy(&mode, shm_host, 4); CHECK(mode == IR_MODE_CLUSTERING, "slot header carries the mode"); }
    svc_call(o, 314, NULL, 0, &c); CHECK(c.result == 0, "CheckFirmwareVersion with a source");
    { uint8_t in[16 + sizeof(IrMomentConfig)] = {0}; in[0] = 2; svc_call(o, 306, in, sizeof in, &c); CHECK(c.result == 0, "RunMomentProcessor on controller 2");
      CHECK(wait_sampling(shm_host, 2, 0, 3), "moment states arrive in slot 2"); const IrMomentState *ms = (const IrMomentState *)(shm_host + 2 * IR_SLOT_SIZE + IR_SLOT_MOMENT);
      CHECK(ms->data[IR_MOMENT_COLS * 2 + 2].avg_intensity > 0, "the block under the disc has intensity"); }
    { id = 0; svc_call(o, 305, &id, 4, &c); CHECK(c.result == 0 && irs_core_running(&core) == 1, "StopImageProcessor stops controller 0 only");
      id = 2; svc_call(o, 318, &id, 4, &c); CHECK(irs_core_running(&core) == 0 && !thread_live, "StopImageProcessorAsync of the last processor ends the pump thread"); }
    { static uint8_t tmem[IR_W * IR_H]; IpcTransferMemory tm = {(uint64_t)(uintptr_t)tmem, sizeof tmem, 3}; uint8_t in[16 + sizeof(IrTransferConfig) + 8] = {0};
      IrTransferConfig cfg; memset(&cfg, 0, sizeof cfg); cfg.origin_format = IR_FMT_160x120; cfg.trimming_format = IR_FMT_80x60; cfg.trim_x = 40; cfg.trim_y = 30; memcpy(in + 16, &cfg, sizeof cfg);
      uint64_t sz = 80 * 60; memcpy(in + 16 + sizeof cfg, &sz, 8);
      memset(&c, 0, sizeof c); c.object = o; c.in = in; c.in_size = sizeof in; c.ncopy = 1; c.copy[0] = handle_new(OBJ_TMEM, &tm); svc_call_c(o, 308, &c); CHECK(c.result == 0, "RunImageTransferProcessor with a transfer memory");
      uint8_t out[IR_W * IR_H]; IpcCall g; memset(&g, 0, sizeof g); g.in = in; g.in_size = 8; svc_set_out_buf(&g, out, sizeof out); svc_call_c(o, 309, &g);
      CHECK(g.result == 0 && g.out_size == 16, "GetImageTransferProcessorState returns the 16-byte state"); CHECK(SVC_OUT(&g, uint64_t, 0) >= 1, "sampling number advances");
      int bright = 0; for (int i = 0; i < 80 * 60; i++) bright += out[i] == 255; CHECK(bright > 0 && out[0] == 0, "the output buffer holds the 80x60 trimmed image with the disc in it");
      CHECK(!memcmp(tmem, out, 80 * 60), "the transfer memory holds the same image");
      id = 0; svc_call(o, 303, NULL, 0, &g); CHECK(irs_core_running(&core) == 0 && !thread_live, "DeactivateIrsensor stops everything"); }
    irs_service_set_source(NULL);
    svc_call(o, 306, NULL, 0, &c); CHECK(c.result == HZ_R_IRSENSOR_NOT_CONNECTED, "removing the source restores the not-connected answers");
    printf("{\"test\":\"svc_irs_sim\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
