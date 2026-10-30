/* control.h - reader of the GLSLC control section that nvnProgramSetShaders receives with each stage's code (portable C, tests/t_control.c).
 *
 * Layout as far as it is known (offsets in bytes from the section start; every field little-endian u32 unless said otherwise):
 *   +0x000 magic 0x98761234        +0x004 1                 +0x008 / +0x00c / +0x010 GLSLC version words (0xf / 0x120 / 0xb on SDK 7..10 outputs)
 *   +0x014 length - 8              +0x020 section length (0x850 on the titles on hand, 0x880 / 0x88c on others, 0x848 on BAHS version 9)
 *   +0x6f0 section length again    +0x6f8 unknown (code size related)   +0x6fc c1 constant bytes   +0x700 c1 offset within the code area
 *   +0x704 code area length        +0x708 code header size (0x30)       +0x70c unknown small count   +0x714 NVN stage (0 vs 1 fs 2 gs 3 tcs 4 tes 5 cs)
 *   +0x748 compute work group x y z (u32 x 3)   +0x754 compute shared memory bytes
 *   +0x034..+0x05b a 40-byte block present in some vertex and fragment outputs (u32, then 33 bytes 01 ff ff ...): purpose unknown
 *   +0x7c8 code length, +0x7d0 a 16-byte digest, +0x7e0 code length, +0x7f4 0xc, +0x7f8..+0x820 fragment output / interpolation words, +0x830 flags
 * b8 scan (2026-10-28, NAPPLE_GFX_CTL_DUMP of 45 s boots, 20624 sections: LAS 152, MK8D A32 1702, MK8D 64-bit 1822, ACNH 2462, SMO 14486; the readers below
 * run over every one: all parse, no transform feedback layout and no tessellation word found, i.e. no false positive):
 *   - +0x020 is not always a multiple of 4 (0x7d1 on SMO / MK8D) and reaches 0x11d0 (LAS): three families 0x7d0/0x7d1, 0x848/0x850, 0x880.
 *   - +0x030..+0x6ef is zero everywhere except 18 sections that carry NV assembly text after the binary part: +0x034 = 0x340, +0x038 = 0xffffff01,
 *     +0x03c..+0x054 = 0xffffffff, +0x058 = 0xff. Those programs capture nothing: that is not a layout (and the consistency checks refuse it, t_control).
 *   - +0x718 is non-zero in fragment sections only (1 or 0x10000; +0x71c = 1, +0x724..+0x730 = 1.0f, +0x734 = 1..8, likely the colour output count):
 *     per-stage state, so the tessellation candidate below is read for tess eval sections alone. Still no tess control / tess eval program in any title.
 *   - geometry sections (ACNH 2, SMO 3) have nothing that matches their SPH (invocations, output topology, max vertices): ACNH's carry +0x828 = 4.
 * Transform feedback layout and tessellation parameters: of the four titles on hand (Link's Awakening, Mario Kart 8 Deluxe, Animal Crossing, Mario
 * Odyssey; RomFS shader archives, the modules' GLSLC outputs and the programs created in 45 s boots scanned 2026-10-27) only geometry programs exist
 * (Odyssey 3, Animal Crossing 2, Mario Kart 1; nothing in them beyond the fields above), no tess control / tess eval program at all, and no vertex or
 * geometry output has data outside the fields above, so their offsets stay unknown: gfx_control_xfb / gfx_control_tess read the candidate region
 * (+0x030..+0x6ef, zero in every output seen, which is where a table of 4 strides + varying lists would fit) only when it is non-zero AND
 * self-consistent, else they report "not present" and the registries of stages.h keep their explicit registrations. */
#ifndef NAPPLE_GFX_CONTROL_H
#define NAPPLE_GFX_CONTROL_H
#include <stddef.h>
#include <stdint.h>
#include "../stages.h"
#ifdef __cplusplus
extern "C" {
#endif

#define GLSLC_CONTROL_MAGIC 0x98761234u
#define GLSLC_CONTROL_MIN 0x760u   /* shortest section seen (the fields up to +0x758 are always there) */
#define GLSLC_CONTROL_MAX 0x2000u   /* longest seen: 0x11d0 (a LAS section with embedded assembly text) */

typedef struct {
    uint32_t length;      /* +0x20, clamped to [GLSLC_CONTROL_MIN, GLSLC_CONTROL_MAX] (0x880 when the word is implausible) */
    uint32_t code_len;    /* +0x704 */
    uint32_t const_off;   /* +0x700 */
    uint32_t const_size;  /* +0x6fc */
    uint32_t stage;       /* +0x714 */
    uint32_t local[3];    /* +0x748 (compute) */
    uint32_t shared_mem;  /* +0x754 (compute) */
    int valid;            /* magic and bounds plausible */
} gfx_control_info;

/* Reads the header fields; returns info.valid. `len` = bytes readable at ctl (0 = trust +0x20). */
int gfx_control_parse(const uint8_t *ctl, size_t len, gfx_control_info *info);
/* The section's own length from +0x20, clamped (what to copy or dump); 0x880 when the word is implausible. */
uint32_t gfx_control_length(const uint8_t *ctl);

/* Transform feedback layout of a vertex / geometry / tess eval section: 1 and *out when the section carries one, 0 when not (or not recognised).
 * Hardware form expected (what the driver programs into the Maxwell TFB methods): per buffer k < 4 a stride in bytes and a varying count n_k, then
 * n_k bytes naming the captured 32-bit components (attribute address / 4: 0x1c..0x1f position, 0x20 + 4 a + c generic a component c, 0xff = a skipped
 * word). See GLSLC_XFB_* below for where this reader looks. */
int gfx_control_xfb(const uint8_t *ctl, size_t len, gfx_xfb_layout *out);
/* Tessellation parameters of a tess eval section: 1 and *out when present, 0 when not recognised (the caller keeps the defaults). */
int gfx_control_tess(const uint8_t *ctl, size_t len, gfx_tess_params *out);

/* Converts the hardware varying form into gfx_xfb_layout (also used by the tests): stride[k] bytes, locs[k][0..count[k]) component indices. 1 on success,
 * 0 when a buffer's run does not fit MAX_XFB_ENTRIES or a stride is not a multiple of 4. */
int gfx_xfb_layout_from_varyings(const uint16_t stride[MAX_XFB_BUFFERS], const uint8_t count[MAX_XFB_BUFFERS], const uint8_t locs[MAX_XFB_BUFFERS][128],
                                 gfx_xfb_layout *out);

/* Candidate placement of the transform feedback block inside the section (hypothesis: not confirmed by a real program, see the header comment).
 * The reader accepts it only when: every stride is a multiple of 4 and <= 2048, every count <= 128, count > 0 for at least one buffer, every named
 * component < 0xc0 (attribute addresses up to 0x2ff) or 0xff, and the named words of a buffer stay below its stride. */
#define GLSLC_XFB_BLOCK_OFF 0x030u          /* u32 stride[4] at +0x030, u32 count[4] at +0x040, u8 locs[4][128] at +0x050 (0x250 bytes in all) */
#define GLSLC_XFB_STRIDE_OFF (GLSLC_XFB_BLOCK_OFF)
#define GLSLC_XFB_COUNT_OFF (GLSLC_XFB_BLOCK_OFF + 0x10u)
#define GLSLC_XFB_LOCS_OFF (GLSLC_XFB_BLOCK_OFF + 0x20u)
/* Tessellation word candidate (hypothesis): +0x718 = Maxwell TESSELLATION_MODE form (bits 0..1 domain 0 isoline 1 triangle 2 quad, bits 4..5 spacing
 * 0 integer 1 fractional odd 2 fractional even, bit 8 clockwise, bit 9 connected) in the stage-specific words after +0x714. Zero = not present. */
#define GLSLC_TESS_MODE_OFF 0x718u

#ifdef __cplusplus
}
#endif
#endif
