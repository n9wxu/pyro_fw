# ── Stage 1: settings needed BEFORE pico_sdk_init() ──────────────────
#
# PICO_BOARD and PICO_BOARD_HEADER_DIRS are consumed by the SDK during
# initialisation, and the flash geometry is consumed by
# pico_fota_bootloader's linker script generation, which runs at
# FetchContent time. All of that happens before add_subdirectory(), which
# is why a board has two CMake entry points rather than one.

set(PYRO_BOARD_KIND pico)  # RP2040 target: uses the Pico SDK and src/hal_common
set(BOARD_DISPLAY_NAME "Pyro MK1B")

# Stock Raspberry Pi Pico module: the SDK already ships boards/pico.h.
set(PICO_BOARD pico CACHE STRING "Board type" FORCE)

set(PYRO_FLASH_SIZE_KB 2048)  # must match PICO_FLASH_SIZE_BYTES for this board
set(PYRO_PFB_FS_KB     984)   # littlefs; see the geometry guard in the top level

# Lua user programs on the one plain J1 pad (GPIO8). See lua_pins.h.
set(PYRO_HAS_LUA 1)

# ── Main-loop budget ─────────────────────────────────────────────────
#
# Worst-case duration of one core0 iteration, in milliseconds. The shared
# main loop derives the watchdog timeout from this (2x) and counts every
# iteration that misses the deadline, so the number is checkable rather
# than asserted: /api/status reports loop_max_us and loop_overruns.
#
# Currently dominated by a 4 KB flash sector erase on the log/OTA path
# (45 ms typical, but this class of part specifies up to 400 ms) and by the
# 10 ms settle in pyro_board.c's pyro_sample(). Deliberately generous: this
# board flies as it is, and a declared budget is how it keeps its current
# behaviour without its sources being touched.
set(PYRO_LOOP_WORST_MS 500)
