/* amiibo crypto core (portable): SHA-256, HMAC-SHA256, AES-128 (CTR), the amiibo key derivation and the tag image <-> internal layout conversion.
 * Written from the publicly documented amiibo scheme as the author remembers it; verified only against the published SHA-256, HMAC and AES test vectors
 * and round trips with a test-only key (see amiibo_core.h). */
#include "amiibo_core.h"
#include <stdio.h>
#include <string.h>

/* ---- SHA-256 ------------------------------------------------------------------------------------------------------------------ */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
} Sha;
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}
static void sha_init(Sha *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->fill = 0;
}
static void sha_update(Sha *s, const void *in, size_t n) {
    const uint8_t *p = in;
    s->len += n;
    while (n) {
        size_t k = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, k);
        s->fill += k;
        p += k;
        n -= k;
        if (s->fill == 64) sha_block(s, s->buf), s->fill = 0;
    }
}
static void sha_final(Sha *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56) sha_update(s, &pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, l, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}
void ami_sha256(const void *in, size_t n, uint8_t out[32]) {
    Sha s;
    sha_init(&s);
    sha_update(&s, in, n);
    sha_final(&s, out);
}
typedef struct {
    Sha in, out;
} Hmac;
static void hmac_init(Hmac *h, const uint8_t *key, size_t keylen) {
    uint8_t k[64] = {0}, pad[64];
    if (keylen > 64) ami_sha256(key, keylen, k);
    else memcpy(k, key, keylen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_init(&h->in);
    sha_update(&h->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha_init(&h->out);
    sha_update(&h->out, pad, 64);
}
static void hmac_final(Hmac *h, uint8_t out[32]) {
    uint8_t inner[32];
    sha_final(&h->in, inner);
    sha_update(&h->out, inner, 32);
    sha_final(&h->out, out);
}
void ami_hmac_sha256(const uint8_t *key, size_t keylen, const void *in, size_t n, uint8_t out[32]) {
    Hmac h;
    hmac_init(&h, key, keylen);
    sha_update(&h.in, in, n);
    hmac_final(&h, out);
}

/* ---- AES-128 (encrypt only; CTR needs nothing else) ------------------------------------------------------------------------------- */
static uint8_t SBOX[256];
static int sbox_ready;
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static void sbox_init(void) { /* generated from the GF(2^8) inverse and the affine transform, not typed in */
    uint8_t p = 1, q = 1;
    do {
        p = p ^ (uint8_t)(p << 1) ^ (uint8_t)((p & 0x80) ? 0x1b : 0);
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^ (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4));
        SBOX[p] = x ^ 0x63;
    } while (p != 1);
    SBOX[0] = 0x63;
    sbox_ready = 1;
}
static void aes_expand(const uint8_t key[16], uint8_t rk[176]) {
    if (!sbox_ready) sbox_init();
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = SBOX[t[1]] ^ rcon;
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[u];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}
static void aes_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[i]];
        /* ShiftRows (column-major state) */
        uint8_t u[16];
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) u[4 * c + rr] = t[4 * ((c + rr) & 3) + rr];
        if (r != 10)
            for (int c = 0; c < 4; c++) {
                uint8_t *x = u + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3;
                x[1] = a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3;
                x[2] = a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3);
                x[3] = (xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) s[i] = u[i] ^ rk[16 * r + i];
    }
    memcpy(out, s, 16);
}
void ami_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t rk[176];
    aes_expand(key, rk);
    aes_block(rk, in, out);
}
void ami_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n) {
    uint8_t rk[176], ctr[16], ks[16];
    aes_expand(key, rk);
    memcpy(ctr, iv, 16);
    for (size_t o = 0; o < n; o += 16) {
        aes_block(rk, ctr, ks);
        for (size_t i = 0; i < 16 && o + i < n; i++) out[o + i] = in[o + i] ^ ks[i];
        for (int i = 15; i >= 0; i--) /* big-endian increment */
            if (++ctr[i]) break;
    }
}

/* ---- key derivation (HMAC-DRBG style, as the amiibo format defines it) ---------------------------------------------------------- */
typedef struct {
    uint8_t hmac_key[16];
    char type_string[14];
    uint8_t rfu, magic_size, magic[16], xor_pad[32];
} MasterKeys;
_Static_assert(sizeof(MasterKeys) == 80, "master key set layout");
typedef struct {
    uint8_t aes_key[16], aes_iv[16], hmac_key[16];
} Derived;

static void drbg(const uint8_t key[16], const uint8_t *seed, size_t seed_size, uint8_t *out, size_t n) {
    uint8_t buf[2 + 0x80];
    memcpy(buf + 2, seed, seed_size);
    uint16_t it = 0;
    for (size_t o = 0; o < n; o += 32, it++) {
        uint8_t block[32];
        buf[0] = (uint8_t)(it >> 8);
        buf[1] = (uint8_t)it;
        ami_hmac_sha256(key, 16, buf, 2 + seed_size, block);
        memcpy(out + o, block, n - o < 32 ? n - o : 32);
    }
}
static size_t prepare_seed(const MasterKeys *m, const uint8_t base[64], uint8_t *out) {
    uint8_t *p = out;
    size_t tl = 0;
    while (tl < 14 && m->type_string[tl]) tl++;
    size_t copy = tl < 14 ? tl + 1 : 14; /* the string including its NUL, or all 14 bytes */
    memcpy(p, m->type_string, copy);
    p += copy;
    size_t lead = 16 - m->magic_size;
    memcpy(p, base, lead);
    p += lead;
    memcpy(p, m->magic, m->magic_size);
    p += m->magic_size;
    memcpy(p, base + 0x10, 16);
    p += 16;
    for (int i = 0; i < 32; i++) p[i] = base[0x20 + i] ^ m->xor_pad[i];
    p += 32;
    return (size_t)(p - out);
}
static void keygen(const uint8_t master[80], const uint8_t *internal, Derived *d) {
    const MasterKeys *m = (const MasterKeys *)master;
    uint8_t base[64] = {0}, seed[0x80];
    memcpy(base, internal + 0x029, 2);
    memcpy(base + 0x10, internal + 0x1d4, 8);
    memcpy(base + 0x18, internal + 0x1d4, 8);
    memcpy(base + 0x20, internal + 0x1e8, 0x20);
    size_t n = prepare_seed(m, base, seed);
    drbg(m->hmac_key, seed, n, (uint8_t *)d, sizeof *d);
}

/* ---- layouts --------------------------------------------------------------------------------------------------------------------- */
static void tag_to_internal(const uint8_t *t, uint8_t *i) {
    memcpy(i + 0x000, t + 0x008, 0x008);
    memcpy(i + 0x008, t + 0x080, 0x020);
    memcpy(i + 0x028, t + 0x010, 0x024);
    memcpy(i + 0x04c, t + 0x0a0, 0x168);
    memcpy(i + 0x1b4, t + 0x034, 0x020);
    memcpy(i + 0x1d4, t + 0x000, 0x008);
    memcpy(i + 0x1dc, t + 0x054, 0x02c);
}
static void internal_to_tag(const uint8_t *i, uint8_t *t) {
    memcpy(t + 0x008, i + 0x000, 0x008);
    memcpy(t + 0x080, i + 0x008, 0x020);
    memcpy(t + 0x010, i + 0x028, 0x024);
    memcpy(t + 0x0a0, i + 0x04c, 0x168);
    memcpy(t + 0x034, i + 0x1b4, 0x020);
    memcpy(t + 0x000, i + 0x1d4, 0x008);
    memcpy(t + 0x054, i + 0x1dc, 0x02c);
}
static void cipher(const Derived *d, const uint8_t *in, uint8_t *out) {
    memcpy(out, in, AMIIBO_TAG_SIZE);
    ami_aes128_ctr(d->aes_key, d->aes_iv, in + 0x02c, out + 0x02c, 0x188);
}
static void data_hmac(const Derived *dk, const uint8_t *plain, const uint8_t *tag_hmac, uint8_t out[32]) {
    Hmac h;
    hmac_init(&h, dk->hmac_key, 16);
    sha_update(&h.in, plain + 0x029, 0x18b);
    sha_update(&h.in, tag_hmac, 0x20);
    sha_update(&h.in, plain + 0x1d4, 0x34);
    hmac_final(&h, out);
}

int amiibo_tag_unpack(const AmiiboKeys *k, const uint8_t tag[AMIIBO_TAG_SIZE], uint8_t plain[AMIIBO_TAG_SIZE]) {
    uint8_t in[AMIIBO_TAG_SIZE] = {0};
    Derived dk, tk;
    tag_to_internal(tag, in);
    keygen(k->data, in, &dk);
    keygen(k->tag, in, &tk);
    cipher(&dk, in, plain);
    uint8_t th[32], dh[32];
    ami_hmac_sha256(tk.hmac_key, 16, plain + 0x1d4, 0x34, th);
    data_hmac(&dk, plain, in + 0x1b4, dh);
    int bad = memcmp(th, in + 0x1b4, 32) != 0;
    bad |= memcmp(dh, in + 0x008, 32) != 0;
    /* the signatures stay in the plain image so it can be re-packed unchanged */
    memcpy(plain + 0x1b4, in + 0x1b4, 0x20);
    memcpy(plain + 0x008, in + 0x008, 0x20);
    return bad ? -1 : 0;
}
void amiibo_tag_pack(const AmiiboKeys *k, const uint8_t plain[AMIIBO_TAG_SIZE], uint8_t tag[AMIIBO_TAG_SIZE]) {
    uint8_t ci[AMIIBO_TAG_SIZE];
    Derived dk, tk;
    keygen(k->tag, plain, &tk);
    keygen(k->data, plain, &dk);
    memcpy(ci, plain, AMIIBO_TAG_SIZE);
    ami_hmac_sha256(tk.hmac_key, 16, plain + 0x1d4, 0x34, ci + 0x1b4);
    data_hmac(&dk, plain, ci + 0x1b4, ci + 0x008);
    uint8_t enc[AMIIBO_TAG_SIZE];
    cipher(&dk, plain, enc);
    memcpy(enc + 0x1b4, ci + 0x1b4, 0x20);
    memcpy(enc + 0x008, ci + 0x008, 0x20);
    memset(tag, 0, AMIIBO_TAG_SIZE);
    internal_to_tag(enc, tag);
}
