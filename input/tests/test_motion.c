#include "check.h"
#include "../src/internal.h"

int main(void) {
    napple_motion m;
    napple_sixaxis_state s;
    const float flat[3] = {0, 0, -1}, zero[3] = {0};
    napple_motion_reset(&m);
    for (int i = 0; i < 200; i++) napple_motion_step(&m, 0.005f, flat, zero, &s);
    CHECK(nearf(s.direction[0], 1, 1e-4f) && nearf(s.direction[4], 1, 1e-4f) && nearf(s.direction[8], 1, 1e-4f), "at rest: identity");
    CHECK(nearf(s.acceleration[2], -1, 1e-6f) && s.attributes == 1, "acceleration passes through");
    /* one full turn about z at 2*pi rad/s for 1 s */
    napple_motion_reset(&m);
    const float wz[3] = {0, 0, 6.2831853f};
    for (int i = 0; i < 200; i++) napple_motion_step(&m, 0.005f, flat, wz, &s);
    CHECK(nearf(s.angle[2], 1.0f, 0.01f) && nearf(s.angular_velocity[2], 1.0f, 1e-4f), "angle %f rev, rate %f rev/s", s.angle[2], s.angular_velocity[2]);
    /* quarter turn about x without gravity correction (acc magnitude out of range): direction = rotation matrix */
    napple_motion_reset(&m);
    const float wx[3] = {1.5707963f, 0, 0}, big[3] = {0, 0, -3};
    napple_motion_step(&m, 0.005f, flat, zero, &s);
    for (int i = 0; i < 200; i++) napple_motion_step(&m, 0.005f, big, wx, &s);
    CHECK(nearf(s.angle[0], 0.25f, 0.005f), "quarter turn angle %f", s.angle[0]);
    CHECK(nearf(s.direction[4], 0, 0.02f) && nearf(fabsf(s.direction[5]), 1, 0.02f), "rotation matrix rows %f %f", s.direction[4], s.direction[5]);
    /* initial tilt from gravity: device on its side (gravity along -y) yields R^T*(0,0,-1) == measured */
    napple_motion_reset(&m);
    const float side[3] = {0, -1, 0};
    for (int i = 0; i < 5; i++) napple_motion_step(&m, 0.005f, side, zero, &s);
    float *r = s.direction, px = -r[6], py = -r[7], pz = -r[8];
    CHECK(nearf(px, 0, 0.02f) && nearf(py, -1, 0.02f) && nearf(pz, 0, 0.02f), "tilt init predicts measured gravity (%f %f %f)", px, py, pz);
    /* drift correction: a gyro bias is pulled back by gravity */
    napple_motion_reset(&m);
    const float bias[3] = {0.05f, 0, 0};
    for (int i = 0; i < 2000; i++) napple_motion_step(&m, 0.005f, flat, bias, &s);
    CHECK(nearf(s.direction[8], 1, 0.01f), "orientation stays level under bias (%f)", s.direction[8]);
    for (int i = 0; i < 9; i++) CHECK(isfinite(s.direction[i]), "finite");
    printf("{\"test\":\"motion\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
