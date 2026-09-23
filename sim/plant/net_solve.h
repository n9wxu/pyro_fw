/*
 * A very small nodal solver, sized for a pyro board.
 *
 * Each board model rebuilds its network every step from the current GPIO
 * levels and then solves it. That is more machinery than reading a level
 * out of a lookup table, and it is worth it for one reason: the interesting
 * readings on these boards are the ones where two elements interact. A
 * channel tied to the bus through a match reads 1037 counts, a channel
 * isolated reads 1214, and a shorted TVS reads 1037 as well -- DESIGN.md
 * 4 derives all three from the same divider algebra. A model that solves
 * the network reproduces them, and reproduces the ones nobody tabulated.
 *
 * Integration is backward Euler:
 *
 *     (G + C/dt) v = (C/dt) v_prev + i
 *
 * Backward rather than forward because the networks are stiff: a 10 nF ADC
 * filter against 330 ohm is a 3.3 us constant, while C_BULK against the
 * bleed is 5 seconds, a spread of 10^6. Forward Euler would need the small
 * step everywhere. Backward Euler is unconditionally stable, so the sim can
 * take the 1 ms step the main loop wants and still settle the fast node
 * correctly.
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
