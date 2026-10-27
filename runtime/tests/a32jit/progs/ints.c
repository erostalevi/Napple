// args: 100 7 3 9
// args: 5000 12345 77 1
unsigned entry(unsigned a, unsigned b, unsigned c, unsigned d) {
    unsigned s = 0, x = b | 1;
    for (unsigned i = 0; i < a; i++) {
        x = x * 1664525u + 1013904223u;
        s += (x >> 7) ^ (i * c);
        s = (s << 3) | (s >> 29);
        if (s & 1) s += d; else s -= i;
    }
    int q = (int)s / (int)(c | 1);
    unsigned long long m = (unsigned long long)s * (unsigned long long)x;
    return s ^ (unsigned)q ^ (unsigned)(m >> 32) ^ (unsigned)m;
}
