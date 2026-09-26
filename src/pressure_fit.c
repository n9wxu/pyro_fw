/*
 * See pressure_fit.h.
 *
 * Times are taken relative to the samples' own mean, and pressures relative to
 * the newest: that keeps the normal equations well conditioned and every
 * number small, so the sums can be float. Only the 3x3 solve is double, a few
 * dozen operations a sample.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_fit.h"
#include <math.h>
#include <string.h>

/* Seconds from the newest sample: negative, and correct across the
 * microsecond clock's wrap because the difference is taken first. */
static float age_s(const uint32_t *t_us, int i, int n) {
    return (float)(int32_t)(t_us[i] - t_us[n - 1]) * 1e-6f;
}

pfit_t pfit_quadratic(const uint32_t *t_us, const int32_t *p_pa, int n) {
    pfit_t f;
    memset(&f, 0, sizeof(f));
    f.n = (uint8_t)(n > 255 ? 255 : n);
    if (n < PFIT_MIN_SAMPLES)
        return f;

    float tm = 0.0f;
    for (int i = 0; i < n; i++)
        tm += age_s(t_us, i, n);
    tm /= (float)n;

    const int32_t ref = p_pa[n - 1];
    float s2 = 0.0f, s3 = 0.0f, s4 = 0.0f, y0 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float u = age_s(t_us, i, n) - tm, y = (float)(p_pa[i] - ref), u2 = u * u;
        s2 += u2;
        s3 += u2 * u;
        s4 += u2 * u2;
        y0 += y;
        y1 += u * y;
        y2 += u2 * y;
    }
    /* Centred, the sum of u is zero:
     *   [ n   0   s2 ] [a]   [y0]
     *   [ 0   s2  s3 ] [b] = [y1]
     *   [ s2  s3  s4 ] [c]   [y2]  */
    double N = n, S2 = s2, S3 = s3, S4 = s4;
    double det = N * (S2 * S4 - S3 * S3) - S2 * (S2 * S2);
    if (det == 0.0)
        return f;
    double Y0 = y0, Y1 = y1, Y2 = y2;
    double a = (Y0 * (S2 * S4 - S3 * S3) + S2 * (Y1 * S3 - Y2 * S2)) / det;
    double b = (N * (Y1 * S4 - S3 * Y2) + S2 * (S3 * Y0 - S2 * Y1)) / det;
    double c = (N * (S2 * Y2 - S3 * Y1) - S2 * (S2 * Y0)) / det;

    /* At the newest sample, u = -tm. */
    double u0 = -(double)tm;
    f.p = (float)((double)ref + a + b * u0 + c * u0 * u0);
    f.pdot = (float)(b + 2.0 * c * u0);
    f.pddot = (float)(2.0 * c);

    float sq = 0.0f, worst = 0.0f;
    for (int i = 0; i < n; i++) {
        float u = age_s(t_us, i, n) - tm;
        float r = (float)(p_pa[i] - ref) - (float)(a + b * u + c * u * u);
        sq += r * r;
        if (fabsf(r) > worst)
            worst = fabsf(r);
    }
    f.rms = sqrtf(sq / (float)n);
    f.worst = worst;
    f.valid = true;
    return f;
}

bool pfit_clean(const pfit_t *f, float sigma_pa) {
    return f->valid && f->rms <= 2.0f * sigma_pa && f->worst <= 4.0f * sigma_pa;
}
