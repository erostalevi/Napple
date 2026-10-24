/* hostobj.h - host-side tables behind descriptor pools. The API layer creates and fills them on the guest thread; the Metal layer reads
 * them on the replay thread. */
#ifndef NAPPLE_GFX_HOSTOBJ_H
#define NAPPLE_GFX_HOSTOBJ_H

#include "obj.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtex { texsnap s; view_t view; } dtex;        /* texture pool entry (s.valid = registered) */
typedef struct gdpool {
    uint32_t magic;
    int is_sampler;
    int n;
    dtex *tex;                            /* n entries (texture pools) */
    samp_par *samp;                       /* n entries (sampler pools) */
    uint8_t *samp_used;
} gdpool;

#ifdef __cplusplus
}
#endif
#endif
