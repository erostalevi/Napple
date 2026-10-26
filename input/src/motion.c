/* Gyro + accelerometer fusion -> Switch six-axis state.

   Orientation is a quaternion integrated from the gyroscope and corrected towards the accelerometer's gravity
   direction (a complementary filter) while the measured acceleration is close to 1 G.  The Switch convention at rest,
   face up on a table, is acceleration (0, 0, -1) G and an identity direction matrix; angular velocity is in
   revolutions per second and `angle` is the integrated rotation in revolutions. */
#include "internal.h"
#include <math.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692f

void napple_motion_reset(napple_motion *m) {
    memset(m, 0, sizeof *m);
    m->q[0] = 1;
}

static void normalise(float q[4]) {
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-9f) {
        q[0] = 1;
        q[1] = q[2] = q[3] = 0;
        return;
    }
    for (int i = 0; i < 4; i++) q[i] /= n;
}

/* rotation matrix of q (sensor -> world), row major */
static void to_matrix(const float q[4], float r[9]) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    r[0] = 1 - 2 * (y * y + z * z);
    r[1] = 2 * (x * y - z * w);
    r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);
    r[4] = 1 - 2 * (x * x + z * z);
    r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);
    r[7] = 2 * (y * z + x * w);
    r[8] = 1 - 2 * (x * x + y * y);
}

void napple_motion_step(napple_motion *m, float dt, const float acc[3], const float gyro[3], napple_sixaxis_state *out) {
    float an = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    if (!m->initialised) {
        /* tilt from gravity: rotate the world's down vector (0,0,-1) onto the measured sensor-frame vector */
        m->initialised = 1;
        m->q[0] = 1;
        m->q[1] = m->q[2] = m->q[3] = 0;
        if (an > 0.5f && an < 1.5f) {
            float v[3] = {acc[0] / an, acc[1] / an, acc[2] / an}, d[3] = {0, 0, -1};
            /* q rotates sensor->world; sensor-frame gravity v must map to world d: axis = v x d */
            float ax[3] = {v[1] * d[2] - v[2] * d[1], v[2] * d[0] - v[0] * d[2], v[0] * d[1] - v[1] * d[0]};
            float dot = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
            m->q[0] = 1 + dot;
            m->q[1] = ax[0];
            m->q[2] = ax[1];
            m->q[3] = ax[2];
            if (m->q[0] < 1e-6f) {
                m->q[0] = 0;
                m->q[1] = 1;
                m->q[2] = 0;
                m->q[3] = 0;
            } /* upside down: 180 deg about x */
            normalise(m->q);
        }
    }
    /* integrate: q += 0.5 * q * (0, gyro) * dt  (gyro in the sensor frame) */
    float w[3] = {gyro[0], gyro[1], gyro[2]};
    float qw = m->q[0], qx = m->q[1], qy = m->q[2], qz = m->q[3];
    m->q[0] += 0.5f * dt * (-qx * w[0] - qy * w[1] - qz * w[2]);
    m->q[1] += 0.5f * dt * (qw * w[0] + qy * w[2] - qz * w[1]);
    m->q[2] += 0.5f * dt * (qw * w[1] - qx * w[2] + qz * w[0]);
    m->q[3] += 0.5f * dt * (qw * w[2] + qx * w[1] - qy * w[0]);
    normalise(m->q);
    /* gravity correction while the accelerometer is roughly only measuring gravity */
    if (an > 0.85f && an < 1.15f) {
        float r[9];
        to_matrix(m->q, r);
        /* predicted sensor-frame gravity = R^T * (0,0,-1) = -(third row of R)  */
        float p[3] = {-r[6], -r[7], -r[8]}, v[3] = {acc[0] / an, acc[1] / an, acc[2] / an};
        float e[3] = {v[1] * p[2] - v[2] * p[1], v[2] * p[0] - v[0] * p[2], v[0] * p[1] - v[1] * p[0]};
        const float kp = 0.8f; /* rad/s per unit error */
        float c[3] = {e[0] * kp, e[1] * kp, e[2] * kp};
        float cw = m->q[0], cx = m->q[1], cy = m->q[2], cz = m->q[3];
        m->q[0] += 0.5f * dt * (-cx * c[0] - cy * c[1] - cz * c[2]);
        m->q[1] += 0.5f * dt * (cw * c[0] + cy * c[2] - cz * c[1]);
        m->q[2] += 0.5f * dt * (cw * c[1] - cx * c[2] + cz * c[0]);
        m->q[3] += 0.5f * dt * (cw * c[2] + cx * c[1] - cy * c[0]);
        normalise(m->q);
    }
    for (int i = 0; i < 3; i++) m->angle[i] += gyro[i] * dt / TWO_PI;
    memset(out, 0, sizeof *out);
    out->delta_time_ns = (uint64_t)(dt * 1e9f);
    for (int i = 0; i < 3; i++) {
        out->acceleration[i] = acc[i];
        out->angular_velocity[i] = gyro[i] / TWO_PI;
        out->angle[i] = m->angle[i];
    }
    to_matrix(m->q, out->direction);
    out->attributes = 1;
}
