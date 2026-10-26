/* soft_double_msl.h - the MSL text of the binary64 emulation: the tokens of soft_double_core.inc stringified. The translator appends
 * sd_msl_source to a shader that uses a double-precision instruction; the same code, compiled as C (soft_double.h), is what the Linux test verifies. */
#ifndef SOFT_DOUBLE_MSL_H
#define SOFT_DOUBLE_MSL_H

#define SD_FN inline
#define SD_STR_(...) #__VA_ARGS__
#define SD_STR(...) SD_STR_(__VA_ARGS__)
#define SD_CODE(...) SD_STR(__VA_ARGS__)
static const char sd_msl_source[] =
#include "soft_double_core.inc"
    ;
#undef SD_CODE
#undef SD_STR
#undef SD_STR_
#undef SD_FN
#undef SD_SIGN
#undef SD_MAG
#undef SD_FRAC
#undef SD_IMPL
#undef SD_INF
#undef SD_NAN
#undef SD_ONE

#endif
