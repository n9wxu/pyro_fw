/*
 * Identity — SIM / WASM board.
 *
 * Not real hardware: this board runs the flight software on the host or in
 * a browser via Emscripten. It has no pins. It is a board because it is a
 * complete alternative implementation of hal.h, selected the same way as
 * any other target:
 *
 *     cmake -B build-sim -DPYRO_BOARD=sim && cmake --build build-sim --target sim
 *     ./scripts/build_wasm.sh                            (Emscripten)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_NAME_STR  "Pyro SIM"
#define BOARD_SHORT_STR "sim"

/* No sensors are fitted; the simulation injects pressure directly. */
#define BOARD_HAS_BMP280 0

#endif
