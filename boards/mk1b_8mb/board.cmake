# Settings the Pico SDK needs before pico_sdk_init(). MK1B's, refitted: U7 an
# 8 MB BY25Q64ESHIG and U5 the base AP2192 [DD-097]. See
# THEORY_OF_OPERATION.md.

include(${CMAKE_CURRENT_LIST_DIR}/../mk1b/board.cmake)

set(BOARD_DISPLAY_NAME "Pyro MK1B-8MB")

# The stock pico header with the flash size changed.
set(PICO_BOARD pyro_mk1b_8mb CACHE STRING "Board type" FORCE)
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/sdk)

set(PYRO_FLASH_SIZE_KB 8192) # BY25Q64ESHIG
set(PYRO_PFB_FS_KB     4096) # littlefs
