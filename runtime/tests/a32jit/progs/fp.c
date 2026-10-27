// args: 50 3 5 7
// args: 400 11 2 1
// float and double arithmetic with conversions: the soft-float helper style code paths of a title (VFPv4)
static float sq(float v) { return v * v; }
unsigned entry(unsigned a, unsigned b, unsigned c, unsigned d) {
    float f = (float)b * 0.25f + 1.0f;
    double g = (double)c * 0.5 + 2.0;
    unsigned acc = 0;
    for (unsigned i = 0; i < a; i++) {
        f = f * 1.0001f + (float)(i & 15) * 0.125f;
        g = g * 0.9999 + (double)f * 1e-3;
        float t = sq(f) - (float)g;
        acc += (unsigned)(int)t ^ (unsigned)(int)(g * 7.0);
        if (f > 1000.0f) f -= 1000.0f;
        if (g > 5000.0) g *= 0.5;
    }
    return acc + (unsigned)(int)f + d + (unsigned)(int)(g + 0.5);
}
