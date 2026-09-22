# ── Stage 1: settings needed BEFORE pico_sdk_init() ──────────────────
#
# PICO_BOARD and PICO_BOARD_HEADER_DIRS are consumed by the SDK during
# initialisation, and the flash geometry is consumed by
# pico_fota_bootloader's linker script generation, which runs at
# FetchContent time. All of that happens before add_subdirectory(), which
# is why a board has two CMake entry points rather than one.

set(PYRO_BOARD_KIND pico)  # RP2040 target: uses the Pico SDK and src/hal_common
set(BOARD_DISPLAY_NAME "Pyro MK1C")

# Bare RP2040: needs its own SDK board header. That header must NOT define
# PICO_DEFAULT_LED_PIN -- see the comment in sdk/pyro_mk1c.h.
set(PICO_BOARD pyro_mk1c CACHE STRING "Board type" FORCE)
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/sdk)

set(PYRO_FLASH_SIZE_KB 16384) # XT25F128FWOIGT-W, 128 Mbit
set(PYRO_PFB_FS_KB     8192)  # littlefs; see the geometry guard in the top level

# Lua user programs on core1, on the four J3 pads (GPIO18-21).
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
# ~42 ms of blocking bias probes in pyro_board.c's pyro_update(). Once the
# log region is pre-erased on the pad and those probes sample on the next
# tick instead of sleeping, this drops to roughly the loop period.
set(PYRO_LOOP_WORST_MS 500)
