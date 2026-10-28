#include "vi_core.h"
#include <string.h>

void vi_init(ViState *v) {
    memset(v, 0, sizeof *v);
    v->display_enabled = 1;
    v->next_layer = 2; /* layer 1 is the display's own */
}
ViLayer *vi_layer(ViState *v, uint64_t id) {
    for (unsigned i = 0; i < v->nlayers; i++)
        if (v->layers[i].id == id) return &v->layers[i];
    if (v->nlayers >= VI_MAX_LAYERS) return NULL;
    ViLayer *l = &v->layers[v->nlayers++];
    memset(l, 0, sizeof *l);
    l->id = id;
    l->scaling = VI_SCALE_TO_WINDOW;
    l->visible = 1;
    return l;
}
ViLayer *vi_layer_create(ViState *v, int managed) {
    ViLayer *l = vi_layer(v, v->next_layer);
    if (!l) return NULL;
    v->next_layer++;
    l->managed = managed != 0;
    return l;
}
int vi_layer_destroy(ViState *v, uint64_t id) {
    for (unsigned i = 0; i < v->nlayers; i++)
        if (v->layers[i].id == id) {
            memmove(&v->layers[i], &v->layers[i + 1], (v->nlayers - i - 1) * sizeof v->layers[0]);
            v->nlayers--;
            return 0;
        }
    return -1;
}
uint32_t vi_set_scaling(ViState *v, uint64_t layer, uint32_t mode) {
    if (mode >= VI_SCALE_COUNT) return VI_R_INVALID_RANGE;
    ViLayer *l = vi_layer(v, layer);
    if (!l) return VI_R_NOT_FOUND;
    l->scaling = mode;
    return 0;
}
int32_t vi_binder_adjust(ViState *v, int32_t id, int32_t delta, int32_t type) {
    if (id < 0 || id >= VI_MAX_BINDERS) return -1;
    int32_t *c = type == 1 ? &v->binder_strong[id] : &v->binder_weak[id];
    *c += delta;
    if (*c < 0) *c = 0;
    return *c;
}

/* ---- IGraphicBufferProducer ---- */
static uint32_t arg_u32(const uint8_t *a, unsigned len, unsigned off, uint32_t def) {
    uint32_t v = def;
    if (a && off + 4 <= len) memcpy(&v, a + off, 4);
    return v;
}
static unsigned put_words(uint8_t *reply, unsigned cap, const int32_t *w, unsigned n) {
    if (cap < n * 4) return 0;
    memcpy(reply, w, n * 4);
    return n * 4;
}
unsigned vi_producer_transact(ViState *v, uint32_t binder, uint32_t code, const uint8_t *args, unsigned args_len, uint8_t *reply, unsigned cap) {
    if (binder >= VI_MAX_BINDERS) binder = 0;
    ViProducer *p = &v->producer[binder];
    if (!p->width) {
        p->width = 1280;
        p->height = 720;
        p->format = 1; /* RGBA8888 */
    }
    int32_t w[5] = {0};
    int32_t slot = (int32_t)arg_u32(args, args_len, 0, 0xffffffffu);
    switch (code) {
        case VI_PT_CONNECT: /* {listener, api, producer controlled by app} -> QueueBufferOutput {width, height, transform hint, pending buffers} */
            p->connected = 1;
            memset(p->slot_state, 0, sizeof p->slot_state);
            w[0] = (int32_t)p->width, w[1] = (int32_t)p->height;
            return put_words(reply, cap, w, 5);
        case VI_PT_DISCONNECT:
            p->connected = 0;
            memset(p->slot_state, 0, sizeof p->slot_state);
            memset(p->slot_requested, 0, sizeof p->slot_requested);
            return put_words(reply, cap, w, 1);
        case VI_PT_QUERY: { /* NATIVE_WINDOW_*: 0 width, 1 height, 2 format, 3 min undequeued buffers, 4 queues to window composer, 5 concrete type (surface), 6 / 7 default size, 8 transform hint */
            uint32_t what = arg_u32(args, args_len, 0, 0xffffffffu);
            int32_t val = 0, st = VI_ST_OK;
            switch (what) {
                case 0: case 6: val = (int32_t)p->width; break;
                case 1: case 7: val = (int32_t)p->height; break;
                case 2: val = (int32_t)p->format; break;
                case 3: val = 1; break;
                case 4: val = 0; break;
                case 5: val = 1; break;
                case 8: val = 0; break;
                default: st = VI_ST_BAD_VALUE; break;
            }
            w[0] = st == VI_ST_OK ? val : 0, w[1] = st;
            return put_words(reply, cap, w, 2);
        }
        case VI_PT_SET_BUFFER_COUNT: { /* {count}: 0 (default) or 2..VI_PROD_SLOTS */
            uint32_t n = (uint32_t)slot;
            if (n != 0 && (n < 2 || n > VI_PROD_SLOTS)) w[0] = VI_ST_BAD_VALUE;
            else p->buffer_count = n;
            return put_words(reply, cap, w, 1);
        }
        case VI_PT_DEQUEUE_BUFFER: { /* {async, width, height, format, usage u64} -> {slot, fence flag (0: none), status} */
            if (!p->connected) {
                w[2] = VI_ST_NO_INIT;
                return put_words(reply, cap, w, 3);
            }
            uint32_t dw = arg_u32(args, args_len, 4, 0), dh = arg_u32(args, args_len, 8, 0), df = arg_u32(args, args_len, 12, 0);
            unsigned lim = p->buffer_count ? p->buffer_count : VI_PROD_SLOTS, i;
            for (i = 0; i < lim; i++)
                if (p->slot_state[i] == 0) break;
            if (i == lim) {
                w[0] = -1, w[2] = VI_ST_BUSY;
                return put_words(reply, cap, w, 3);
            }
            if (dw) p->width = dw;
            if (dh) p->height = dh;
            if (df) p->format = df;
            p->slot_state[i] = 1;
            w[0] = (int32_t)i;
            return put_words(reply, cap, w, 3);
        }
        case VI_PT_REQUEST_BUFFER: /* {slot}: the graphic buffer object itself is not produced (flag 0), status only */
            if (slot < 0 || slot >= VI_PROD_SLOTS || p->slot_state[slot] != 1) w[0] = VI_ST_BAD_VALUE;
            else p->slot_requested[slot] = 1;
            return put_words(reply, cap, w, 1);
        case VI_PT_QUEUE_BUFFER: /* {slot, input...} -> QueueBufferOutput + status */
            if (!p->connected) w[4] = VI_ST_NO_INIT;
            else if (slot < 0 || slot >= VI_PROD_SLOTS || p->slot_state[slot] != 1) w[4] = VI_ST_BAD_VALUE;
            else {
                p->slot_state[slot] = 2;
                p->frame_counter++;
                p->slot_state[slot] = 0; /* nothing consumes it: the buffer is free again at once */
            }
            w[0] = (int32_t)p->width, w[1] = (int32_t)p->height;
            return put_words(reply, cap, w, 5);
        case VI_PT_CANCEL_BUFFER: /* {slot, fence} */
            if (!p->connected) w[0] = VI_ST_NO_INIT;
            else if (slot < 0 || slot >= VI_PROD_SLOTS || p->slot_state[slot] != 1) w[0] = VI_ST_BAD_VALUE;
            else p->slot_state[slot] = 0;
            return put_words(reply, cap, w, 1);
        case VI_PT_DETACH_BUFFER: case VI_PT_DETACH_NEXT_BUFFER: case VI_PT_ATTACH_BUFFER: case VI_PT_SET_SIDEBAND_STREAM:
        case VI_PT_ALLOCATE_BUFFERS: case VI_PT_SET_PREALLOCATED_BUFFER:
            return put_words(reply, cap, w, 1); /* nothing to detach / attach / preallocate: success */
        default: return 0;
    }
}
