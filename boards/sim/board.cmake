# ── SIM / WASM board ─────────────────────────────────────────────────
#
# A host board, not an RP2040 target. It supplies its own complete
# implementation of hal.h (hal_sim.c) rather than implementing
# src/board_if.h on top of src/hal_common, because there is no Pico SDK
# underneath it.
#
# This is the composability src/board_if.h describes: "porting to a
# different MCU family means supplying a new common HAL alongside a new
# board directory; the board's CMakeLists.txt chooses which one it links."
# The sim is the degenerate case -- it links no common HAL at all.

set(PYRO_BOARD_KIND host)              # not "pico": skip the SDK entirely
set(BOARD_DISPLAY_NAME "Pyro SIM")

# Sources the host/WASM targets compile in place of the board support
# libraries a pico-kind board would contribute.
set(PYRO_HOST_HAL_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/hal_sim.c
)
