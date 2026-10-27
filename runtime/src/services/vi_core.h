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
typedef struct {
    uint8_t display_enabled; /* SetDisplayEnabled; starts on */
    ViLayer layers[VI_MAX_LAYERS];
    unsigned nlayers;
    uint64_t next_layer;                                                /* ids handed out by CreateStrayLayer / CreateManagedLayer */
    int32_t binder_strong[VI_MAX_BINDERS], binder_weak[VI_MAX_BINDERS]; /* indexed by binder id */
} ViState;

void vi_init(ViState *v);
/* The layer with this id; one the title names without having created it (OpenLayer on the display's first layer) is registered on first use. NULL only when the table is full. */
ViLayer *vi_layer(ViState *v, uint64_t id);
ViLayer *vi_layer_create(ViState *v, int managed);                  /* a fresh layer id (2, 3, ...) */
int vi_layer_destroy(ViState *v, uint64_t id);                      /* 0, or -1 when unknown */
uint32_t vi_set_scaling(ViState *v, uint64_t layer, uint32_t mode); /* 0, VI_R_INVALID_RANGE for an unknown mode, VI_R_NOT_FOUND when the table is full */
/* Binder reference counts: type 0 = weak, 1 = strong (android::IBinder). Counts never go below zero; returns the new count or -1 for an id out of range. */
int32_t vi_binder_adjust(ViState *v, int32_t id, int32_t delta, int32_t type);
