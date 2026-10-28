/* vi core (portable): the display/layer bookkeeping behind vi:u / vi:m / vi:s and the IHOSBinderDriver reference counts. The host presents through its own window, so
 * nothing here reaches the screen; the state is what a title can read back or that a firmware applet sets and expects to stay set. */
#pragma once
#include <stdint.h>

#define VI_MAX_LAYERS 16
#define VI_MAX_BINDERS 16
#define VI_R_INVALID_RANGE (114u | (5u << 9)) /* [unverified] module 114 (vi), "invalid range" */
#define VI_R_NOT_FOUND (114u | (7u << 9))     /* [unverified] */
enum { VI_SCALE_FREEZE, VI_SCALE_TO_WINDOW, VI_SCALE_AND_CROP, VI_SCALE_NONE, VI_SCALE_PRESERVE_ASPECT, VI_SCALE_COUNT };

typedef struct {
    uint64_t id;
    uint32_t scaling; /* VI_SCALE_*, default ScaleToWindow */
    float x, y;
    uint64_t w, h;
    int64_t z;
    uint8_t visible, managed, stack;
} ViLayer;
/* IGraphicBufferProducer bookkeeping (android::Surface). The graphics back end never produces buffers through it, so no pixels move: the state only keeps
 * the answers consistent (a slot is dequeued before it is queued or cancelled, a connect precedes the rest, disconnect clears the lot). Transaction codes and
 * parcel layouts are the author's memory of the Android IGraphicBufferProducer / Switch fork (unverified). */
#define VI_PROD_SLOTS 3
enum { VI_PT_REQUEST_BUFFER = 1, VI_PT_SET_BUFFER_COUNT = 2, VI_PT_DEQUEUE_BUFFER = 3, VI_PT_DETACH_BUFFER = 4, VI_PT_DETACH_NEXT_BUFFER = 5, VI_PT_ATTACH_BUFFER = 6,
       VI_PT_QUEUE_BUFFER = 7, VI_PT_CANCEL_BUFFER = 8, VI_PT_QUERY = 9, VI_PT_CONNECT = 10, VI_PT_DISCONNECT = 11, VI_PT_SET_SIDEBAND_STREAM = 13,
       VI_PT_ALLOCATE_BUFFERS = 14, VI_PT_SET_PREALLOCATED_BUFFER = 15 };
#define VI_ST_OK 0
#define VI_ST_BAD_VALUE (-22)  /* -EINVAL */
#define VI_ST_NO_INIT (-19)    /* -ENODEV: not connected */
#define VI_ST_BUSY (-16)       /* -EBUSY: every slot is dequeued */
typedef struct {
    uint8_t connected;
    uint8_t slot_state[VI_PROD_SLOTS]; /* 0 free, 1 dequeued, 2 queued */
    uint8_t slot_requested[VI_PROD_SLOTS];
    uint32_t width, height, format;
    uint32_t buffer_count;
    uint64_t frame_counter; /* queueBuffer calls */
} ViProducer;
typedef struct {
    uint8_t display_enabled; /* SetDisplayEnabled; starts on */
    ViLayer layers[VI_MAX_LAYERS];
    unsigned nlayers;
    uint64_t next_layer;                                                /* ids handed out by CreateStrayLayer / CreateManagedLayer */
    int32_t binder_strong[VI_MAX_BINDERS], binder_weak[VI_MAX_BINDERS]; /* indexed by binder id */
    ViProducer producer[VI_MAX_BINDERS];                                /* IGraphicBufferProducer state per binder id */
} ViState;

/* One transaction. `args` is the parcel's data after the interface token (args_len bytes). The reply data (without the parcel header) is written to `reply` (max
 * `cap` bytes) and ends with the android status word; returns the number of bytes written, 0 for a code that is not known (the caller then answers a bad-value status). */
unsigned vi_producer_transact(ViState *v, uint32_t binder, uint32_t code, const uint8_t *args, unsigned args_len, uint8_t *reply, unsigned cap);

void vi_init(ViState *v);
/* The layer with this id; one the title names without having created it (OpenLayer on the display's first layer) is registered on first use. NULL only when the table is full. */
ViLayer *vi_layer(ViState *v, uint64_t id);
ViLayer *vi_layer_create(ViState *v, int managed);                  /* a fresh layer id (2, 3, ...) */
int vi_layer_destroy(ViState *v, uint64_t id);                      /* 0, or -1 when unknown */
uint32_t vi_set_scaling(ViState *v, uint64_t layer, uint32_t mode); /* 0, VI_R_INVALID_RANGE for an unknown mode, VI_R_NOT_FOUND when the table is full */
/* Binder reference counts: type 0 = weak, 1 = strong (android::IBinder). Counts never go below zero; returns the new count or -1 for an id out of range. */
int32_t vi_binder_adjust(ViState *v, int32_t id, int32_t delta, int32_t type);
