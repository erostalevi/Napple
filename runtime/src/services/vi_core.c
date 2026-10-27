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
