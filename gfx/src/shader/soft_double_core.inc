/* soft_double_core.inc - IEEE 754 binary64 on 64-bit integers: the single source of the Maxwell double-precision emulation.
 *
 * This file is included twice, never compiled on its own:
 *   - soft_double.h   defines SD_CODE(...) as its argument, so the code below is compiled as C (the host reference, tested on Linux against the CPU's double);
 *   - soft_double_msl.h defines SD_CODE(...) as a string, so the very same tokens are emitted into the Metal shader (Metal has no double).
 * Keep to the subset both languages share: uint = 32 bits, ulong = 64 bits, no pointers, no address spaces, no unary minus on unsigned values,
 * every shift count below the operand width. Constants live in macros and are expanded before the code is stringified.
 *
 * A binary64 is a ulong holding the raw bits (a Maxwell register pair: low word in the even register, high word in the odd one).
 * Rounding modes follow the Maxwell field: 0 RN (nearest even), 1 RM (toward -inf), 2 RP (toward +inf), 3 RZ (toward zero).
 * Denormals are fully supported (no flush). Every NaN result is the canonical quiet NaN 0x7FFFFFFFFFFFFFFF (the hardware does not propagate payloads).
 *
 * Internal form: a finite nonzero value is sig * 2^(ex - 1084) with sig normalised so bit 62 is set (so 1.0 is sig 2^62, ex 1022: ex is the biased exponent
 * minus one, the convention of Berkeley SoftFloat's roundPack, with ten spare low bits below the 52 fraction bits for round and sticky).
 */
// clang-format off
#define SD_SIGN 0x8000000000000000ul
#define SD_MAG 0x7FFFFFFFFFFFFFFFul
#define SD_FRAC 0x000FFFFFFFFFFFFFul
#define SD_IMPL 0x0010000000000000ul
#define SD_INF 0x7FF0000000000000ul
#define SD_NAN 0x7FFFFFFFFFFFFFFFul
#define SD_ONE 0x3FF0000000000000ul

SD_CODE(
SD_FN uint sd_clz64(ulong x) {
    uint hi = (uint)(x >> 32);
    return hi != 0u ? clz(hi) : 32u + clz((uint)x);
}

SD_FN ulong sd_shr_jam(ulong x, uint n) {
    if (n == 0u) return x;
    if (n >= 64u) return x != 0ul ? 1ul : 0ul;
    return (x >> n) | ((x << (64u - n)) != 0ul ? 1ul : 0ul);
}

SD_FN uint sd_isnan(ulong a) { return (a & SD_MAG) > SD_INF ? 1u : 0u; }
SD_FN uint sd_isinf(ulong a) { return (a & SD_MAG) == SD_INF ? 1u : 0u; }
SD_FN uint sd_iszero(ulong a) { return (a & SD_MAG) == 0ul ? 1u : 0u; }
SD_FN ulong sd_neg(ulong a) { return a ^ SD_SIGN; }
SD_FN ulong sd_abs(ulong a) { return a & SD_MAG; }

/* normalised significand and exponent of a finite nonzero value (subnormals are normalised) */
SD_FN ulong sd_nsig(ulong a) {
    uint e = (uint)(a >> 52) & 0x7FFu;
    ulong f = a & SD_FRAC;
    if (e != 0u) return (f | SD_IMPL) << 10;
    ulong s = f << 10;
    return s << (sd_clz64(s) - 1u);
}
SD_FN int sd_nex(ulong a) {
    uint e = (uint)(a >> 52) & 0x7FFu;
    if (e != 0u) return (int)e - 1;
    ulong s = (a & SD_FRAC) << 10;
    return 0 - (int)(sd_clz64(s) - 1u);
}

/* round and pack: sig has bit 62 set (or is smaller for a result already known to be subnormal), ten round bits below the fraction */
SD_FN ulong sd_round_pack(uint sign, int ex, ulong sig, uint rm) {
    ulong incr = 0x200ul;
    if (rm != 0u) incr = rm == 3u ? 0ul : (((rm == 1u) == (sign != 0u)) ? 0x3FFul : 0ul);
    ulong rb = sig & 0x3FFul;
    if ((uint)ex >= 0x7FDu) {
        if (ex < 0) {
            sig = sd_shr_jam(sig, (uint)(0 - ex));
            ex = 0;
            rb = sig & 0x3FFul;
        } else if (ex > 0x7FD || 0x8000000000000000ul <= sig + incr) {
            return ((ulong)sign << 63) + (incr == 0ul ? 0x7FEFFFFFFFFFFFFFul : SD_INF);
        }
    }
    sig = (sig + incr) >> 10;
    if (rm == 0u && rb == 0x200ul) sig = sig & ~1ul;
    return ((ulong)sign << 63) + ((ulong)(uint)ex << 52) + sig;
}

SD_FN ulong sd_mulhi64(ulong a, ulong b) {
    ulong a0 = a & 0xFFFFFFFFul, a1 = a >> 32, b0 = b & 0xFFFFFFFFul, b1 = b >> 32;
    ulong p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    ulong mid = (p00 >> 32) + (p01 & 0xFFFFFFFFul) + (p10 & 0xFFFFFFFFul);
    return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

SD_FN ulong sd_add(ulong a, ulong b, uint rm) {
    uint ea = (uint)(a >> 52) & 0x7FFu, eb = (uint)(b >> 52) & 0x7FFu;
    uint sa = (uint)(a >> 63), sb = (uint)(b >> 63);
    if (sd_isnan(a) != 0u || sd_isnan(b) != 0u) return SD_NAN;
    if (ea == 0x7FFu) return (eb == 0x7FFu && sa != sb) ? SD_NAN : a;
    if (eb == 0x7FFu) return b;
    if (sd_iszero(a) != 0u) {
        if (sd_iszero(b) != 0u) return sa == sb ? a : (rm == 1u ? SD_SIGN : 0ul);
        return b;
    }
    if (sd_iszero(b) != 0u) return a;
    int xa = sd_nex(a), xb = sd_nex(b);
    ulong ma = sd_nsig(a), mb = sd_nsig(b);
    if (xa < xb || (xa == xb && ma < mb)) {
        int xt = xa;
        xa = xb;
        xb = xt;
        ulong mt = ma;
        ma = mb;
        mb = mt;
        uint st = sa;
        sa = sb;
        sb = st;
    }
    mb = sd_shr_jam(mb, (uint)(xa - xb));
    ulong s;
    int x = xa;
    if (sa == sb) {
        s = ma + mb;
        if (s >= 0x8000000000000000ul) {
            s = sd_shr_jam(s, 1u);
            x = x + 1;
        }
    } else {
        s = ma - mb;
        if (s == 0ul) return rm == 1u ? SD_SIGN : 0ul;
        uint z = sd_clz64(s) - 1u;
        s = s << z;
        x = x - (int)z;
    }
    return sd_round_pack(sa, x, s, rm);
}

SD_FN ulong sd_sub(ulong a, ulong b, uint rm) { return sd_add(a, b ^ SD_SIGN, rm); }

SD_FN ulong sd_mul(ulong a, ulong b, uint rm) {
    uint sign = (uint)((a ^ b) >> 63);
    if (sd_isnan(a) != 0u || sd_isnan(b) != 0u) return SD_NAN;
    if (sd_isinf(a) != 0u || sd_isinf(b) != 0u) {
        if (sd_iszero(a) != 0u || sd_iszero(b) != 0u) return SD_NAN;
        return ((ulong)sign << 63) | SD_INF;
    }
    if (sd_iszero(a) != 0u || sd_iszero(b) != 0u) return (ulong)sign << 63;
    int xa = sd_nex(a), xb = sd_nex(b);
    ulong ma = sd_nsig(a), mb = sd_nsig(b);
    ulong hi = sd_mulhi64(ma, mb), lo = ma * mb;
    ulong sig;
    int x;
    if (hi >= 0x2000000000000000ul) {
        sig = (hi << 1) | (lo >> 63) | ((lo << 1) != 0ul ? 1ul : 0ul);
        x = xa + xb - 1021;
    } else {
        sig = (hi << 2) | (lo >> 62) | ((lo << 2) != 0ul ? 1ul : 0ul);
        x = xa + xb - 1022;
    }
    return sd_round_pack(sign, x, sig, rm);
}

/* 128-bit (h:l) shifted right by n with the shifted-out bits ORed into bit 0 of the low word; the two halves come back from two calls */
SD_FN ulong sd_shr128_hi(ulong h, uint n) {
    if (n == 0u) return h;
    if (n >= 64u) return 0ul;
    return h >> n;
}
SD_FN ulong sd_shr128_lo(ulong h, ulong l, uint n) {
    if (n == 0u) return l;
    if (n < 64u) return (l >> n) | (h << (64u - n)) | ((l << (64u - n)) != 0ul ? 1ul : 0ul);
    if (n == 64u) return h | (l != 0ul ? 1ul : 0ul);
    if (n < 128u) return (h >> (n - 64u)) | (((h << (128u - n)) | l) != 0ul ? 1ul : 0ul);
    return (h | l) != 0ul ? 1ul : 0ul;
}
SD_FN ulong sd_shl128_hi(ulong h, ulong l, uint n) {
    if (n == 0u) return h;
    if (n < 64u) return (h << n) | (l >> (64u - n));
    return l << (n - 64u);
}
SD_FN ulong sd_shl128_lo(ulong l, uint n) {
    if (n == 0u) return l;
    if (n < 64u) return l << n;
    return 0ul;
}

/* fused multiply-add: a * b + c with a single rounding */
SD_FN ulong sd_fma(ulong a, ulong b, ulong c, uint rm) {
    uint ps = (uint)((a ^ b) >> 63), cs = (uint)(c >> 63);
    if (sd_isnan(a) != 0u || sd_isnan(b) != 0u || sd_isnan(c) != 0u) return SD_NAN;
    if (sd_isinf(a) != 0u || sd_isinf(b) != 0u) {
        if (sd_iszero(a) != 0u || sd_iszero(b) != 0u) return SD_NAN;
        if (sd_isinf(c) != 0u && cs != ps) return SD_NAN;
        return ((ulong)ps << 63) | SD_INF;
    }
    if (sd_isinf(c) != 0u) return c;
    if (sd_iszero(a) != 0u || sd_iszero(b) != 0u) {
        if (sd_iszero(c) != 0u) return ps == cs ? ((ulong)ps << 63) : (rm == 1u ? SD_SIGN : 0ul);
        return c;
    }
    int xp = sd_nex(a) + sd_nex(b);
    ulong ma = sd_nsig(a), mb = sd_nsig(b);
    ulong ph = sd_mulhi64(ma, mb), pl = ma * mb;
    if (ph >= 0x2000000000000000ul) {
        ph = (ph << 1) | (pl >> 63);
        pl = pl << 1;
        xp = xp - 1021;
    } else {
        ph = (ph << 2) | (pl >> 62);
        pl = pl << 2;
        xp = xp - 1022;
    }
    if (sd_iszero(c) != 0u) return sd_round_pack(ps, xp, ph | (pl != 0ul ? 1ul : 0ul), rm);
    ulong ch = sd_nsig(c), cl = 0ul;
    int xc = sd_nex(c);
    ulong h1 = ph, l1 = pl, h2 = ch, l2 = cl;
    int x1 = xp, x2 = xc;
    uint s1 = ps, s2 = cs;
    if (xc > xp || (xc == xp && (ch > ph || (ch == ph && cl > pl)))) {
        h1 = ch;
        l1 = cl;
        x1 = xc;
        s1 = cs;
        h2 = ph;
        l2 = pl;
        x2 = xp;
        s2 = ps;
    }
    uint d = (uint)(x1 - x2);
    ulong t2h = sd_shr128_hi(h2, d), t2l = sd_shr128_lo(h2, l2, d);
    ulong rh, rl;
    if (s1 == s2) {
        rl = l1 + t2l;
        rh = h1 + t2h + (rl < l1 ? 1ul : 0ul);
        if (rh >= 0x8000000000000000ul) {
            ulong nh = sd_shr128_hi(rh, 1u);
            rl = sd_shr128_lo(rh, rl, 1u);
            rh = nh;
            x1 = x1 + 1;
        }
    } else {
        rl = l1 - t2l;
        rh = h1 - t2h - (l1 < t2l ? 1ul : 0ul);
        if (rh == 0ul && rl == 0ul) return rm == 1u ? SD_SIGN : 0ul;
        uint z = rh != 0ul ? sd_clz64(rh) - 1u : 63u + sd_clz64(rl);
        ulong nh = sd_shl128_hi(rh, rl, z);
        rl = sd_shl128_lo(rl, z);
        rh = nh;
        x1 = x1 - (int)z;
    }
    return sd_round_pack(s1, x1, rh | (rl != 0ul ? 1ul : 0ul), rm);
}

/* comparison with the Maxwell condition codes: 0 F, 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 NUM, 8 NAN, 9..14 the same six unordered-or-true, 15 T */
SD_FN uint sd_cmp(ulong a, ulong b, uint op) {
    uint un = (sd_isnan(a) != 0u || sd_isnan(b) != 0u) ? 1u : 0u;
    ulong ma = a & SD_MAG, mb = b & SD_MAG;
    uint eq = ((ma == 0ul && mb == 0ul) || a == b) ? 1u : 0u;
    uint lt = 0u;
    if (eq == 0u) {
        if (((a ^ b) >> 63) != 0ul) lt = (a >> 63) != 0ul ? 1u : 0u;
        else lt = ((a >> 63) != 0ul ? (ma > mb) : (ma < mb)) ? 1u : 0u;
    }
    uint gt = (eq == 0u && lt == 0u) ? 1u : 0u;
    uint ok = un == 0u ? 1u : 0u;
    uint r = 0u;
    uint o = op & 7u;
    if (op == 0u) r = 0u;
    else if (op == 15u) r = 1u;
    else if (op == 7u) r = ok;
    else if (op == 8u) r = un;
    else {
        if (o == 1u) r = lt;
        else if (o == 2u) r = eq;
        else if (o == 3u) r = (lt | eq);
        else if (o == 4u) r = gt;
        else if (o == 5u) r = eq == 0u ? 1u : 0u;
        else r = lt == 0u ? 1u : 0u;
        r = op < 7u ? (r & ok) : (r | un);
    }
    return r;
}

/* DMNMX: the non-NaN operand wins; for equal zeros max prefers +0 and min prefers -0 */
SD_FN ulong sd_minmax(ulong a, ulong b, uint wantmax) {
    if (sd_isnan(a) != 0u) return sd_isnan(b) != 0u ? SD_NAN : b;
    if (sd_isnan(b) != 0u) return a;
    if (sd_cmp(a, b, 2u) != 0u) return wantmax != 0u ? (a & b) : (a | b);
    uint lt = sd_cmp(a, b, 1u);
    if (wantmax != 0u) return lt != 0u ? b : a;
    return lt != 0u ? a : b;
}

/* round to an integral value in binary64: mode 0 nearest even, 1 floor, 2 ceil, 3 trunc */
SD_FN ulong sd_rint(ulong a, uint mode) {
    if (sd_isnan(a) != 0u) return SD_NAN;
    uint e = (uint)(a >> 52) & 0x7FFu;
    if (e >= 1075u) return a;
    ulong sign = a & SD_SIGN;
    if (e < 1023u) {
        if ((a & SD_MAG) == 0ul) return a;
        uint up = 0u;
        if (mode == 0u) up = (e == 1022u && (a & SD_FRAC) != 0ul) ? 1u : 0u;
        else if (mode == 1u) up = sign != 0ul ? 1u : 0u;
        else if (mode == 2u) up = sign == 0ul ? 1u : 0u;
        return sign | (up != 0u ? SD_ONE : 0ul);
    }
    ulong unit = 1ul << (1075u - e);
    ulong mask = unit - 1ul;
    ulong rem = a & mask;
    ulong t = a & ~mask;
    if (rem == 0ul) return a;
    ulong hunit = unit >> 1;
    uint up = 0u;
    if (mode == 0u) up = (rem > hunit || (rem == hunit && (t & unit) != 0ul)) ? 1u : 0u;
    else if (mode == 1u) up = sign != 0ul ? 1u : 0u;
    else if (mode == 2u) up = sign == 0ul ? 1u : 0u;
    return up != 0u ? t + unit : t;
}

/* integer -> binary64 (also the exact half of 32-bit conversions); sgn selects a two's complement source */
SD_FN ulong sd_from_int(ulong x, uint sgn, uint rm) {
    uint sign = (sgn != 0u && (x >> 63) != 0ul) ? 1u : 0u;
    ulong m = sign != 0u ? 0ul - x : x;
    if (m == 0ul) return 0ul;
    uint lead = 63u - sd_clz64(m);
    ulong sig = lead <= 62u ? (m << (62u - lead)) : sd_shr_jam(m, 1u);
    return sd_round_pack(sign, (int)lead + 1022, sig, rm);
}

/* binary64 -> integer of nbits (8..64), rounding with mode (0 RN, 1 RM, 2 RP, 3 RZ) and saturating; NaN gives 0. A negative signed result comes back sign-extended to 64 bits. */
SD_FN ulong sd_to_int(ulong a, uint mode, uint sgn, uint nbits) {
    if (sd_isnan(a) != 0u) return 0ul;
    ulong limp = sgn != 0u ? ((1ul << (nbits - 1u)) - 1ul) : (nbits >= 64u ? ~0ul : ((1ul << nbits) - 1ul));
    ulong limn = sgn != 0u ? (1ul << (nbits - 1u)) : 0ul;
    ulong r = sd_rint(a, mode);
    uint e = (uint)(r >> 52) & 0x7FFu;
    ulong mag = 0ul;
    if (e >= 1087u) mag = ~0ul;
    else if (e >= 1023u) {
        ulong m = (r & SD_FRAC) | SD_IMPL;
        uint s = e - 1023u;
        mag = s >= 52u ? (m << (s - 52u)) : (m >> (52u - s));
    }
    if ((r >> 63) != 0ul) return 0ul - (mag > limn ? limn : mag);
    return mag > limp ? limp : mag;
}

/* binary32 <-> binary64 */
SD_FN ulong sd_from_f32(uint x) {
    ulong sign = (ulong)(x >> 31) << 63;
    uint e = (x >> 23) & 0xFFu, f = x & 0x7FFFFFu;
    if (e == 0xFFu) return f != 0u ? SD_NAN : (sign | SD_INF);
    if (e != 0u) return sign | ((ulong)(e + 896u) << 52) | ((ulong)f << 29);
    if (f == 0u) return sign;
    uint lead = 31u - clz(f);
    return sign | ((ulong)(lead + 874u) << 52) | (((ulong)f << (52u - lead)) & SD_FRAC);
}

SD_FN uint sd_round_pack32(uint sign, int ex, ulong sig, uint rm) {
    ulong incr = 0x40ul;
    if (rm != 0u) incr = rm == 3u ? 0ul : (((rm == 1u) == (sign != 0u)) ? 0x7Ful : 0ul);
    ulong rb = sig & 0x7Ful;
    if ((uint)ex >= 0xFDu) {
        if (ex < 0) {
            sig = sd_shr_jam(sig, (uint)(0 - ex));
            ex = 0;
            rb = sig & 0x7Ful;
        } else if (ex > 0xFD || 0x80000000ul <= sig + incr) {
            return (sign << 31) + (incr == 0ul ? 0x7F7FFFFFu : 0x7F800000u);
        }
    }
    sig = (sig + incr) >> 7;
    if (rm == 0u && rb == 0x40ul) sig = sig & ~1ul;
    return (sign << 31) + ((uint)ex << 23) + (uint)sig;
}

SD_FN uint sd_to_f32(ulong a, uint rm) {
    uint sign = (uint)(a >> 63);
    if (sd_isnan(a) != 0u) return 0x7FFFFFFFu;
    if (sd_isinf(a) != 0u) return (sign << 31) | 0x7F800000u;
    if (sd_iszero(a) != 0u) return sign << 31;
    return sd_round_pack32(sign, sd_nex(a) - 896, sd_shr_jam(sd_nsig(a), 32u), rm);
}

/* integer -> binary32 for 64-bit sources (single rounding) */
SD_FN uint sd_int_to_f32(ulong x, uint sgn, uint rm) {
    uint sign = (sgn != 0u && (x >> 63) != 0ul) ? 1u : 0u;
    ulong m = sign != 0u ? 0ul - x : x;
    if (m == 0ul) return 0u;
    uint lead = 63u - sd_clz64(m);
    ulong sig = lead <= 62u ? (m << (62u - lead)) : sd_shr_jam(m, 1u);
    return sd_round_pack32(sign, (int)lead + 1022 - 896, sd_shr_jam(sig, 32u), rm);
}
)
// clang-format on
