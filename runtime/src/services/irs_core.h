/* irs core: the IR camera simulator (portable). Replaces "no camera": a frame source delivers 320x240 8-bit near-IR intensity frames and the
 * processors the games run (moment, clustering, image transfer) are computed from them.
 *
 *  - IrFrameSource is the contract the frontend implements: a webcam adapter on the Mac (irs_adapter_rgb_to_ir converts a camera frame), or the
 *    built-in scripted/synthetic sources used by tests and headless runs (NAPPLE_IR_SOURCE).
 *  - Camera access is requested only when a game starts a processor (request_access); with no source installed every processor reports the IR sensor
 *    as not connected, exactly as before.
 *  - The state the games read is written into the irsensor shared memory (layout below, a seqlock per processor).
 * Struct layouts follow the author's memory of the public description; none of it is verified on a console or against a title. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* NpadIdType (0..7 = players 1..8, 0x10 other, 0x20 handheld) -> IrCameraHandle (u8 controller id, u8 npad type, 2 bytes padding);
 * returns 0 and fills *out, or -1 for an id that is not an npad. Controller ids follow the public hid numbering (handheld 8, other 9). */
int irs_camera_handle(uint32_t npad_id, uint32_t *out);

#define IR_W 320
#define IR_H 240
#define IR_NCAM 10 /* controller ids 0..9 */
#define IR_MOMENT_COLS 8
#define IR_MOMENT_ROWS 6
#define IR_MOMENT_BLOCKS (IR_MOMENT_COLS * IR_MOMENT_ROWS) /* 0x30 */
#define IR_MAX_CLUSTERS 16

/* ---- frame source (implemented by the frontend / by tests) ---------------------------------------------------------------------- */
enum { IR_ACCESS_DENIED = -1, IR_ACCESS_PENDING = 0, IR_ACCESS_GRANTED = 1 };
typedef struct {
    void *ctx;
    const char *name;
    /* Called when a game starts a processor, never before. Returns IR_ACCESS_*; PENDING means a permission prompt is showing: the source calls
     * done(cbctx, granted) later (on any thread). May be NULL (access always granted). */
    int (*request_access)(void *ctx, void (*done)(void *cbctx, int granted), void *cbctx);
    int (*start)(void *ctx); /* optional; 0 = ok */
    void (*stop)(void *ctx); /* optional */
    /* Copies the newest frame (IR_W*IR_H bytes, row major, 0 = dark) into out. Returns 1 for a frame newer than the last call, 0 for none, -1 on error. */
    int (*get_frame)(void *ctx, uint8_t *out, uint64_t *timestamp_ns);
} IrFrameSource;

/* Built-in sources. spec grammar (NAPPLE_IR_SOURCE): "blob:x,y,r[,dx,dy]" a bright disc at (x,y) of radius r that moves by (dx,dy) per frame and bounces off
 * the edges; "dark" an all-dark frame; "flat:v" a uniform frame of intensity v. Returns 0 and fills *out (a static instance: one per process). */
int irs_source_from_spec(const char *spec, IrFrameSource *out);

/* ---- webcam adapter (the Mac frontend feeds camera frames through this) ---------------------------------------------------------- */
typedef struct {
    int mirror;    /* flip horizontally */
    int stretch;   /* contrast-stretch between the 1st and 99th percentile of the luminance */
    int threshold; /* 0 = off; else luminance below it becomes 0 */
    int binarize;  /* with threshold: luminance at or above it becomes 255 */
} IrAdapterOpts;
/* RGB(A)/BGR(A) camera frame in, near-IR-like grayscale out (IR_W x IR_H): luminance, a centred 4:3 crop, box/bilinear resampling, then the options.
 * channels is 3 or 4; bgr is 1 for BGR(A) byte order. Returns 0, or -1 for a bad argument. */
int irs_adapter_rgb_to_ir(const uint8_t *src, int w, int h, int stride, int channels, int bgr, const IrAdapterOpts *opts, uint8_t *out);

/* ---- processors ---------------------------------------------------------------------------------------------------------------- */
typedef struct {
    int16_t x, y, w, h;
} IrRect;
typedef struct {
    uint64_t exposure_ns;
    uint32_t light_target; /* 0 all, 1 ambient/far, 2 near, 3 none */
    uint32_t gain;
    uint8_t negative; /* use the negative image */
    uint8_t pad[7];
} IrCameraConfig;
enum { IR_PRE_NONE = 0, IR_PRE_BINARIZE = 1, IR_PRE_CUTOFF = 2 };
typedef struct {
    IrCameraConfig cam;
    IrRect window; /* w or h <= 0: the whole frame */
    uint32_t preprocess, threshold;
} IrMomentConfig;
typedef struct {
    IrCameraConfig cam;
    IrRect window;
    uint32_t pixel_min, pixel_max, intensity_min; /* pixel_max 0 = no limit */
    uint8_t external_light_filter, pad[3];
} IrClusteringConfig;
enum { IR_FMT_320x240 = 0, IR_FMT_160x120, IR_FMT_80x60, IR_FMT_40x30, IR_FMT_20x15 };
typedef struct {
    IrCameraConfig cam;
    uint32_t origin_format, trimming_format;
    uint16_t trim_x, trim_y;
    uint8_t external_light_filter, pad[3];
} IrTransferConfig;
_Static_assert(sizeof(IrCameraConfig) == 24 && sizeof(IrMomentConfig) == 40 && sizeof(IrClusteringConfig) == 48 && sizeof(IrTransferConfig) == 40,
               "config layouts");

typedef struct {
    float avg_intensity, cx, cy; /* centroid in window pixel coordinates */
} IrMomentData;
typedef struct {
    uint8_t avg_intensity, pad[3];
    float cx, cy; /* centroid in frame pixel coordinates */
    uint32_t pixel_count;
    int16_t bx, by, bw, bh; /* bounding rectangle */
} IrClusterData;
typedef struct {
    uint64_t seq, timestamp_ns, sampling_number;
    uint32_t ambient_noise, pad;
    IrMomentData data[IR_MOMENT_BLOCKS];
} IrMomentState;
typedef struct {
    uint64_t seq, timestamp_ns, sampling_number;
    uint8_t count, pad[7];
    IrClusterData data[IR_MAX_CLUSTERS];
} IrClusterState;
typedef struct {
    uint64_t sampling_number;
    uint32_t ambient_noise, pad;
} IrTransferState;
_Static_assert(sizeof(IrMomentData) == 12 && sizeof(IrClusterData) == 24 && sizeof(IrMomentState) == 608 && sizeof(IrClusterState) == 416, "state layouts");

/* Shared memory (0x8000 bytes): one 0x800-byte slot per camera (controller id). Slot: +0x00 u32 mode, u32 pad, u64 frames processed; +0x10 IrMomentState;
 * +0x280 IrClusterState. A writer sets seq odd, fills the state, sets seq even (+2); a reader retries while seq is odd or changed. */
#define IR_SHM_SIZE 0x8000
#define IR_SLOT_SIZE 0x800
#define IR_SLOT_MOMENT 0x10
#define IR_SLOT_CLUSTER 0x280

/* Pure computations on one frame (exposed for tests). */
void ir_moment_compute(const uint8_t *frame, const IrMomentConfig *cfg, IrMomentData out[IR_MOMENT_BLOCKS]);
/* Returns the number of clusters written (largest first). */
int ir_cluster_compute(const uint8_t *frame, const IrClusteringConfig *cfg, IrClusterData out[IR_MAX_CLUSTERS]);
/* The image the transfer processor delivers; out must hold IR_W*IR_H bytes. Returns the byte count and sets *w / *h. */
size_t ir_image_make(const uint8_t *frame, const IrTransferConfig *cfg, uint8_t *out, int *w, int *h);
uint32_t ir_ambient_noise(const uint8_t *frame); /* 0 low, 1 medium, 2 high */

/* ---- the simulator ------------------------------------------------------------------------------------------------------------- */
typedef enum { IR_MODE_NONE = 0, IR_MODE_MOMENT, IR_MODE_CLUSTERING, IR_MODE_TRANSFER, IR_MODE_OTHER } IrMode;
typedef struct {
    IrMode mode;
    union {
        IrMomentConfig moment;
        IrClusteringConfig cluster;
        IrTransferConfig transfer;
    } cfg;
    uint8_t *xfer_dst; /* the game's transfer memory (host pointer), or NULL */
    size_t xfer_size;
    uint8_t *image; /* latest transfer image */
    size_t image_size;
    int image_w, image_h;
    uint64_t sampling;
    uint32_t noise;
    uint64_t frames;
} IrsCam;
typedef struct {
    IrsCam cam[IR_NCAM];
    IrFrameSource src;
    int have_src, started, asked;
    int access; /* IR_ACCESS_*: granted, denied, or pending (the initial value) */
    uint8_t frame[IR_W * IR_H];
    uint64_t frame_ts;
    int have_frame;
} IrsCore;

void irs_core_init(IrsCore *c);
void irs_core_free(IrsCore *c);
void irs_core_set_source(IrsCore *c, const IrFrameSource *src); /* NULL removes it (processors then report "not connected") */
int irs_core_has_source(const IrsCore *c);
/* Access callback target for sources that answer request_access asynchronously. */
void irs_core_access_done(void *core, int granted);
/* Starts a processor on a camera. cfg points at the matching config struct (NULL = defaults). Returns 0 or HZ_R_IRSENSOR_NOT_CONNECTED (no source, access denied,
 * unknown camera). xfer is the transfer memory for IR_MODE_TRANSFER. */
uint32_t irs_core_run(IrsCore *c, unsigned cam, IrMode mode, const void *cfg, uint8_t *xfer, size_t xfer_size);
void irs_core_stop(IrsCore *c, unsigned cam);
int irs_core_running(const IrsCore *c); /* number of cameras with a processor */
/* Pulls a frame from the source and updates every running processor: writes the shared memory states (shm may be NULL) and the transfer memory. Returns
 * the number of cameras updated (0 when no new frame arrived). */
int irs_core_pump(IrsCore *c, uint8_t *shm, uint64_t now_ns);
/* GetImageTransferProcessorState: copies the latest image into dst (up to cap bytes) and reports the state. Returns the bytes copied, or -1 when the camera is not
 * running an image transfer. */
long irs_core_image(IrsCore *c, unsigned cam, uint8_t *dst, size_t cap, IrTransferState *st);

/* ---- service-level API (irs.c) for the frontend and tests ----------------------------------------------------------------------- */
/* Installs the frame source of the process (NULL removes it). Without one, NAPPLE_IR_SOURCE (see irs_source_from_spec) is read at the first ActivateIrsensor;
 * with neither, every processor reports the IR sensor as not connected. */
void irs_service_set_source(const IrFrameSource *src);
