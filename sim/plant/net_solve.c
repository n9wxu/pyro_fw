/*
 * Nodal solver. See net_solve.h for why it exists and why it integrates
 * backward.
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_solve.h"

#include <math.h>
#include <string.h>

/* Below this a resistance is treated as a short and above it as an open.
 * The bounds matter because board models compute resistances from switch
 * states: a conducting AO3400A is 30 mohm and an open one is "infinite",
 * and the solver has to stay conditioned across that. */
#define R_MIN 1e-4   /* 100 uohm: below this, a short                 */
#define R_MAX 1e12   /* 1 Tohm:   above this, no connection at all    */

void net_begin(net_t *net, int n) {
    net->n = (n > NET_MAX_NODES) ? NET_MAX_NODES : n;
    memset(net->g, 0, sizeof(net->g));
    memset(net->i, 0, sizeof(net->i));
    memset(net->c, 0, sizeof(net->c));
    memset(net->pinned, 0, sizeof(net->pinned));
    memset(net->pin_v, 0, sizeof(net->pin_v));
    /* net->v is deliberately not cleared: it is the integrator state. */
}

static bool usable(double ohms) {
    return ohms > 0.0 && ohms < R_MAX && isfinite(ohms);
}

void net_res_to_gnd(net_t *net, int node, double ohms) {
    if (!usable(ohms))
        return;
    if (ohms < R_MIN)
        ohms = R_MIN;
    net->g[node][node] += 1.0 / ohms;
}

void net_res(net_t *net, int a, int b, double ohms) {
    if (!usable(ohms))
        return;
    if (ohms < R_MIN)
        ohms = R_MIN;
    double g = 1.0 / ohms;
    net->g[a][a] += g;
    net->g[b][b] += g;
    net->g[a][b] -= g;
    net->g[b][a] -= g;
}

void net_src_through_res(net_t *net, int node, double volts, double ohms) {
    if (!usable(ohms))
        return;
    if (ohms < R_MIN)
        ohms = R_MIN;
    net->g[node][node] += 1.0 / ohms;
    net->i[node] += volts / ohms;
}

void net_cap_to_gnd(net_t *net, int node, double farads) {
    if (farads > 0.0)
        net->c[node] += farads;
}

void net_pin(net_t *net, int node, double volts) {
    net->pinned[node] = true;
    net->pin_v[node] = volts;
}

void net_solve(net_t *net, double dt_s) {
    const int n = net->n;
    double a[NET_MAX_NODES][NET_MAX_NODES + 1];

    if (dt_s <= 0.0)
        dt_s = 1e-9;

    for (int r = 0; r < n; r++) {
        if (net->pinned[r]) {
            /* Replace the row with v[r] = pin_v[r]. The column is not
             * eliminated from the other rows; instead the pinned node's
             * contribution is moved to their right-hand side below, which
             * keeps the matrix the same shape. */
            for (int c = 0; c <= n; c++)
                a[r][c] = 0.0;
            a[r][r] = 1.0;
            a[r][n] = net->pin_v[r];
            continue;
        }
        double gc = net->c[r] / dt_s;
        for (int c = 0; c < n; c++)
            a[r][c] = net->g[r][c];
        a[r][r] += gc;
        a[r][n] = net->i[r] + gc * net->v[r];
    }

    /* Move each pinned node's known voltage to the right-hand side of the
     * rows that couple to it. */
    for (int p = 0; p < n; p++) {
        if (!net->pinned[p])
            continue;
        for (int r = 0; r < n; r++) {
            if (r == p || net->pinned[r])
                continue;
            a[r][n] -= a[r][p] * net->pin_v[p];
            a[r][p] = 0.0;
        }
    }

    /* Gaussian elimination with partial pivoting. n is at most 4. */
    for (int col = 0; col < n; col++) {
        int piv = col;
        for (int r = col + 1; r < n; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col]))
                piv = r;
        if (fabs(a[piv][col]) < 1e-18) {
            /* A node with no path to ground and no capacitance: genuinely
             * floating. Leave it where it was rather than producing a NaN
             * that would propagate into every later step. */
            a[col][col] = 1.0;
            a[col][n] = net->v[col];
            for (int c = 0; c < n; c++)
                if (c != col)
                    a[col][c] = 0.0;
            piv = col;
        }
        if (piv != col)
            for (int c = 0; c <= n; c++) {
                double t = a[col][c];
                a[col][c] = a[piv][c];
                a[piv][c] = t;
            }
        for (int r = col + 1; r < n; r++) {
            double f = a[r][col] / a[col][col];
            if (f == 0.0)
                continue;
            for (int c = col; c <= n; c++)
                a[r][c] -= f * a[col][c];
        }
    }

    for (int r = n - 1; r >= 0; r--) {
        double s = a[r][n];
        for (int c = r + 1; c < n; c++)
            s -= a[r][c] * net->v[c];
        net->v[r] = (fabs(a[r][r]) < 1e-18) ? net->v[r] : s / a[r][r];
        if (!isfinite(net->v[r]))
            net->v[r] = 0.0;
    }
}

double net_current(const net_t *net, int a, int b, double ohms) {
    if (!usable(ohms))
        return 0.0;
    if (ohms < R_MIN)
        ohms = R_MIN;
    return (net->v[a] - net->v[b]) / ohms;
}

double net_current_to_gnd(const net_t *net, int node, double ohms) {
    if (!usable(ohms))
        return 0.0;
    if (ohms < R_MIN)
        ohms = R_MIN;
    return net->v[node] / ohms;
}
