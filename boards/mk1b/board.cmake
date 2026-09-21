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
