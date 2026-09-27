# Settings the Pico SDK needs before pico_sdk_init(); the sources are in
# CMakeLists.txt. See boards/README.md for why a board has both.

set(PYRO_BOARD_KIND pico)
set(BOARD_DISPLAY_NAME "Pyro MK1B")

set(PICO_BOARD pico CACHE STRING "Board type" FORCE) # the Pico module itself

set(PYRO_FLASH_SIZE_KB 2048)
set(PYRO_PFB_FS_KB     984)  # littlefs

set(PYRO_HAS_LUA 1) # the J1 pad, GPIO8

# The worst loop iteration; the watchdog is twice it. See
# THEORY_OF_OPERATION.md "Build".
set(PYRO_LOOP_WORST_MS 500)
