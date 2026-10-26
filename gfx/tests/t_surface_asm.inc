/* Assembler for the surface instructions (fields as documented at the top of shader/surface.inc). Included by t_xlate.m. */
enum { SD_1D, SD_BUFFER, SD_1D_ARRAY, SD_2D, SD_2D_ARRAY, SD_3D };
enum { SZ_U8, SZ_S8, SZ_U16, SZ_S16, SZ_32, SZ_64, SZ_128 };
enum { CL_IGN, CL_TRAP, CL_CLAMP, CL_ZERO };
enum { SA_ADD, SA_MIN, SA_MAX, SA_INC, SA_DEC, SA_AND, SA_OR, SA_XOR, SA_EXCH, SA_CAS };
enum { ST_U32, ST_S32, ST_U64, ST_F32, ST_S64 };
static uint64_t i_su(unsigned top, int d, int a, int rb, int clamp, int ba, int f28, int p, int f32, int index, int dim) {
    return ((uint64_t)top << 52) | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)rb << 20) | ((uint64_t)clamp << 47) | ((uint64_t)ba << 26) |
           ((uint64_t)f28 << 28) | ((uint64_t)p << 31) | ((uint64_t)f32 << 32) | ((uint64_t)index << 36) | ((uint64_t)dim << 49) | PT_BITS;
}
static uint64_t i_suld(int d, int a, int dim, int index, int size, int clamp) { return i_su(0xeb0, d, a, 0, clamp, 0, size, 0, 0, index, dim); }
static uint64_t i_suld_ba(int d, int a, int dim, int index, int size, int clamp) { return i_su(0xeb0, d, a, 0, clamp, 1, size, 0, 0, index, dim); }
static uint64_t i_suld_p(int d, int a, int dim, int index, int mask, int clamp) { return i_su(0xeb0, d, a, 0, clamp, 0, 0, 1, mask, index, dim); }
static uint64_t i_suld_b(int d, int a, int hreg, int dim, int size, int clamp) { return i_su(0xeb1, d, a, hreg, clamp, 0, size, 0, 0, 0, dim); }
static uint64_t i_sust(int d, int a, int dim, int index, int size, int clamp) { return i_su(0xeb2, d, a, 0, clamp, 0, size, 0, 0, index, dim); }
static uint64_t i_sust_p(int d, int a, int dim, int index, int mask, int clamp) { return i_su(0xeb2, d, a, 0, clamp, 0, 0, 1, mask, index, dim); }
static uint64_t i_sust_b(int d, int a, int hreg, int dim, int size, int clamp) { return i_su(0xeb3, d, a, hreg, clamp, 0, size, 0, 0, 0, dim); }
static uint64_t i_suatom(int d, int a, int b, int op, int type, int dim, int index, int clamp) {
    return i_su(0xea0, d, a, b, clamp, 0, op, 0, type, index, dim);
}
static uint64_t i_sured(int d, int a, int b, int op, int type, int dim, int index, int clamp) {
    return i_su(0xeb4, d, a, b, clamp, 0, op, 0, type, index, dim);
}
/* MOV Rd, c[cb][off]: how a bindless handle gets into a register */
static uint64_t i_mov_cb(int d, int cb, int off) { return 0x4c98000000000000ull | (uint64_t)d | ((uint64_t)(off / 4) << 20) | ((uint64_t)cb << 34) | PT_BITS; }
