/*
 * A nodal solver sized for a pyro board, rebuilt by the board model every
 * step from the pad levels. Backward Euler, (G + C/dt) v = (C/dt) v_prev + i,
 * because the networks are stiff (microsecond ADC filters beside
 * millisecond bleeds) and it is stable at any step.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_SOLVE_H
#define NET_SOLVE_H

#include <stdbool.h>

#define NET_MAX_NODES 4

typedef struct {
    int    n;                                  /* nodes in use            */
    double g[NET_MAX_NODES][NET_MAX_NODES];    /* conductance matrix, S   */
    double i[NET_MAX_NODES];                   /* current injected, A     */
    double c[NET_MAX_NODES];                   /* node capacitance, F     */
    double v[NET_MAX_NODES];                   /* solved voltage, V       */
    bool   pinned[NET_MAX_NODES];              /* held by an ideal source */
    double pin_v[NET_MAX_NODES];
} net_t;

/* Clear g, i, c and the pins. Node voltages are KEPT: they are the state
 * that carries across a step. */
void net_begin(net_t *net, int n);

/* Conductance from a node to ground, and between two nodes. Both take
 * ohms; a resistance of zero or less is ignored rather than producing an
 * infinity, so a caller can pass a computed value without guarding it. */
void net_res_to_gnd(net_t *net, int node, double ohms);
void net_res(net_t *net, int a, int b, double ohms);

/* A voltage source behind a resistance, which is how every real stimulus on
 * these boards is connected: a GPIO through a bias resistor, a pack through
 * an eFuse. Applied as its Norton equivalent. */
void net_src_through_res(net_t *net, int node, double volts, double ohms);

/* Capacitance from a node to ground. */
void net_cap_to_gnd(net_t *net, int node, double farads);

/* Hold a node at a voltage regardless of the rest of the network. For an
 * element that really is a stiff source -- a saturated switch tying a node
 * to ground, or an eFuse in its slew-rate-limited ramp, where the board
 * model is computing the ramp itself and the network only has to follow. */
void net_pin(net_t *net, int node, double volts);

/* Solve for the end of a dt-second step and write the result into v. */
void net_solve(net_t *net, double dt_s);

/* Current flowing from node a into node b through a resistance, using the
 * voltages of the last solve. Sign is positive for a -> b. */
double net_current(const net_t *net, int a, int b, double ohms);

/* Same, from a node into ground. */
double net_current_to_gnd(const net_t *net, int node, double ohms);

#endif
