/* Stub of the libvpx decoder API movie.c uses (syntax check only). */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int vpx_codec_err_t;
typedef const void *vpx_codec_iter_t;
enum { VPX_CODEC_OK = 0, VPX_IMG_FMT_I420 = 0x102, VPX_PLANE_Y = 0, VPX_PLANE_U = 1, VPX_PLANE_V = 2, VPX_CR_FULL_RANGE = 1,
       VPX_CS_BT_601 = 1, VPX_CS_SMPTE_170 = 3, VPX_CS_SMPTE_240 = 4, VPX_CS_BT_709 = 2 };
typedef struct { int fmt, cs, range; unsigned d_w, d_h; unsigned char *planes[4]; int stride[4]; } vpx_image_t;
typedef struct { void *priv; } vpx_codec_ctx_t;
typedef struct { unsigned threads, w, h; } vpx_codec_dec_cfg_t;
typedef struct vpx_codec_iface vpx_codec_iface_t;
vpx_codec_iface_t *vpx_codec_vp9_dx(void);
vpx_codec_err_t vpx_codec_dec_init(vpx_codec_ctx_t *, vpx_codec_iface_t *, const vpx_codec_dec_cfg_t *, long);
vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *, const uint8_t *, unsigned, void *, long);
vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *, vpx_codec_iter_t *);
vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t *);
const char *vpx_codec_error_detail(const vpx_codec_ctx_t *);
const char *vpx_codec_err_to_string(vpx_codec_err_t);
const char *vpx_codec_version_str(void);
