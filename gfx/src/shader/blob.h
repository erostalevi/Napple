/* blob.h - shader blobs: the code of one program stage, copied out of guest memory when the program is created. */
#ifndef NAPPLE_GFX_BLOB_H
#define NAPPLE_GFX_BLOB_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct shader_blob {
    uint64_t hash;
    int stage;                 /* NVN stage: 0 vs, 1 fs, 5 cs */
    uint8_t *code;             /* instruction bytes incl. NVN header + SPH for graphics stages */
    uint32_t code_size;
    uint8_t *consts;           /* compiler constants (c1) */
    uint32_t const_size;
    uint32_t local[3];         /* compute work group size */
    uint32_t local_mem;        /* bytes */
} shader_blob;
const shader_blob *shader_blob_find(uint64_t hash);
#ifdef __cplusplus
}
#endif
#endif
