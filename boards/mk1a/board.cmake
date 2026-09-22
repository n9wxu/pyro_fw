# ── Stage 1: settings needed BEFORE pico_sdk_init() ──────────────────
#
# See boards/mk1c/board.cmake for why a board has two CMake entry points.

set(PYRO_BOARD_KIND pico)  # RP2040 target: uses the Pico SDK and src/hal_common
set(BOARD_DISPLAY_NAME "Pyro MK1A")

# Bare RP2040 with a 16MB part, so it cannot reuse PICO_BOARD=pico (which
# declares 2MB). See sdk/pyro_mk1a.h.
set(PICO_BOARD pyro_mk1a CACHE STRING "Board type" FORCE)
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/sdk)

set(PYRO_FLASH_SIZE_KB 16384) # W25Q128JVS, 128 Mbit
set(PYRO_PFB_FS_KB     8192)  # littlefs; see the geometry guard in the top level

# No Lua: J3/J4 are pyro terminals and J6 is the serial expansion header, so
# this board has no free user pins to grant a script.

# ── Main-loop budget ─────────────────────────────────────────────────
#
# Worst-case duration of one core0 iteration, in milliseconds. The shared
# main loop derives the watchdog timeout from this (2x) and counts every
# iteration that misses the deadline, so the number is checkable rather
# than asserted: /api/status reports loop_max_us and loop_overruns.
#
# Same conservative starting value as the other boards, dominated by a 4 KB
# flash sector erase on the log/OTA path (45 ms typical, up to 400 ms for
# this class of part). Measure with loop_max_us on real hardware and bring
# it down.
set(PYRO_LOOP_WORST_MS 500)
