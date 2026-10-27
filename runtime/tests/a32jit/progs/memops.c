// args: 200 3 5 0
// args: 1000 17 9 1
// memcpy / memset / strlen / memcmp style loops on a stack buffer: the compiler vectorises them with NEON
static void copy(unsigned char *d, const unsigned char *s, unsigned n) { for (unsigned i = 0; i < n; i++) d[i] = s[i]; }
static unsigned len(const unsigned char *p) { unsigned n = 0; while (p[n]) n++; return n; }
static unsigned sum(const unsigned char *p, unsigned n) { unsigned s = 0; for (unsigned i = 0; i < n; i++) s += p[i] * 3u; return s; }
unsigned entry(unsigned a, unsigned b, unsigned c, unsigned d) {
    unsigned char x[1024], y[1024];
    unsigned n = a > 1000 ? 1000 : a;
    for (unsigned i = 0; i < 1024; i++) x[i] = (unsigned char)((i * b + c) | (i > n ? 0 : 1));
    x[n] = 0;
    copy(y, x, n + 1);
    for (unsigned i = 0; i < n; i += 7) y[i] ^= (unsigned char)d;
    unsigned diff = 0;
    for (unsigned i = 0; i < n; i++) diff += (x[i] != y[i]);
    return len(y) * 65537u + sum(y, n) + diff;
}
