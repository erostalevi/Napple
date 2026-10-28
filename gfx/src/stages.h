/* stages.h - per-program parameters of the stages beyond vertex / fragment that the shader code itself does not carry (docs/architecture/gfx.md section 13).
 *
 * On the console NVN programs these from the program's GLSLC control section: the transform feedback layout (which output components go to which buffer at
 * which offset, and each buffer's stride) and the tessellation parameters of the evaluation program (domain, spacing, winding). Where that data sits in the
 * control section is not known yet, so both are registered here, keyed by the shader blob hash of the stage that owns them (vertex program for the layout,
 * tess eval program for the parameters). The replay reads them per draw. Without a registration: no transform feedback is captured (a warning names the
 * program once) and tessellation uses the defaults below. Thread-safe (the API thread registers, the replay thread reads). */
#ifndef NAPPLE_GFX_STAGES_H
#define NAPPLE_GFX_STAGES_H
#include <stdint.h>
#include "shader/maxwell.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t count;                       /* valid entries */
    uint16_t stride[MAX_XFB_BUFFERS];    /* bytes per captured vertex of each buffer (multiple of 4; 0 = buffer unused) */
    ShaderXfbEntry e[MAX_XFB_ENTRIES];
} gfx_xfb_layout;

enum { TESS_SPACING_EQUAL = 0, TESS_SPACING_FRACTIONAL_ODD = 1, TESS_SPACING_FRACTIONAL_EVEN = 2 };
enum { TESS_WINDING_CCW = 0, TESS_WINDING_CW = 1 };
typedef struct {
    uint8_t domain;  /* TESS_DOMAIN_*: default TRIANGLE (NAPPLE_GFX_TESS_DOMAIN=quad|triangle changes the default) */
    uint8_t spacing; /* TESS_SPACING_*: default EQUAL */
    uint8_t winding; /* TESS_WINDING_*: default CCW */
} gfx_tess_params;

void gfx_xfb_set_layout(uint64_t vs_hash, const gfx_xfb_layout *l);    /* l == NULL removes it */
int gfx_xfb_get_layout(uint64_t vs_hash, gfx_xfb_layout *out);         /* 1 when registered */
void gfx_tess_set_params(uint64_t tes_hash, const gfx_tess_params *p); /* p == NULL removes it */
int gfx_tess_get_params(uint64_t tes_hash, gfx_tess_params *out);      /* always fills *out (defaults when not registered); 1 when registered */
/* The same keyed by an initialized NVNprogram (its vertex / tess eval stage); 0 when the program has no such stage. */
int gfx_program_set_xfb_layout(const void *nvn_program, const gfx_xfb_layout *l);
int gfx_program_set_tess_params(const void *nvn_program, const gfx_tess_params *p);

#ifdef __cplusplus
}
#endif
#endif
