# The pyro flight computer: firmware for the board PYRO_BOARD names, the host
# test suites and the simulator.

# 3.18: FetchContent_Declare(SOURCE_SUBDIR) with FetchContent_MakeAvailable.
cmake_minimum_required(VERSION 3.18)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

# ====================================================================================
# Target board selection  [PYRO_BOARD]
#
# Boards are self-contained packages under boards/<name>/. Nothing in this
# file names a board, so adding one never edits this file:
#
#     1. cp -r boards/reference boards/<name>
#     2. Implement src/board_if.h, src/pyro.h and src/pressure_sensor.h
#     3. Edit boards/<name>/board.cmake and boards/<name>/CMakeLists.txt
#     4. cmake -B build-<name> -DPYRO_BOARD=<name> && cmake --build build-<name>
#
# Each board has TWO CMake entry points, because the SDK needs some settings
# before it is initialised and others only after:
#
#     board.cmake     included HERE, before the SDK is initialised. Sets
#                     PYRO_BOARD_KIND, BOARD_DISPLAY_NAME, PICO_BOARD,
#                     PICO_BOARD_HEADER_DIRS and the flash geometry, which
#                     pico_fota_bootloader consumes when it generates its
#                     linker script at FetchContent time.
#     CMakeLists.txt  add_subdirectory()'d after pico_sdk_init(). Declares
#                     sources and exports the target `pyro_board`.
# ====================================================================================
file(GLOB _pyro_board_dirs RELATIVE ${CMAKE_CURRENT_LIST_DIR}/boards ${CMAKE_CURRENT_LIST_DIR}/boards/*)
set(PYRO_BOARDS_AVAILABLE "")
foreach(_d ${_pyro_board_dirs})
    if(IS_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/boards/${_d}
       AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/boards/${_d}/board.cmake)
        list(APPEND PYRO_BOARDS_AVAILABLE ${_d})
    endif()
endforeach()

set(PYRO_BOARD mk1b CACHE STRING "Target board (a directory under boards/)")
set_property(CACHE PYRO_BOARD PROPERTY STRINGS ${PYRO_BOARDS_AVAILABLE})

if(NOT PYRO_BOARD IN_LIST PYRO_BOARDS_AVAILABLE)
    message(FATAL_ERROR
        "Unknown board '${PYRO_BOARD}'.\n"
        "Available: ${PYRO_BOARDS_AVAILABLE}\n"
        "Configure with -DPYRO_BOARD=<name>")
endif()

set(PYRO_BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/boards/${PYRO_BOARD})
include(${PYRO_BOARD_DIR}/board.cmake)

# A board declares its KIND, and the top level branches on the kind rather
# than on any board name. Two kinds exist:
#
#   pico  an RP2040 target: Pico SDK, src/hal_common + src/board_if.h,
#         a .uf2 flashed behind pico_fota_bootloader
#   host  runs on the build machine or in a browser via Emscripten, and
#         supplies its own complete hal.h implementation
#
# Adding a board of an existing kind never edits this file.
if(NOT DEFINED PYRO_BOARD_KIND)
    set(PYRO_BOARD_KIND pico)
endif()

set(_required BOARD_DISPLAY_NAME)
if(PYRO_BOARD_KIND STREQUAL "pico")
    list(APPEND _required PYRO_FLASH_SIZE_KB PYRO_PFB_FS_KB PYRO_LOOP_WORST_MS)
elseif(PYRO_BOARD_KIND STREQUAL "host")
    list(APPEND _required PYRO_HOST_HAL_SOURCES)
else()
    message(FATAL_ERROR "boards/${PYRO_BOARD}/board.cmake: PYRO_BOARD_KIND must be 'pico' or 'host'")
endif()
foreach(_r ${_required})
    if(NOT DEFINED ${_r})
        message(FATAL_ERROR "boards/${PYRO_BOARD}/board.cmake must set ${_r}")
    endif()
endforeach()

if(PYRO_BOARD_KIND STREQUAL "pico")
    message(STATUS "Pyro target board: ${PYRO_BOARD} (${BOARD_DISPLAY_NAME}), kind=pico, "
                   "flash ${PYRO_FLASH_SIZE_KB}KB, littlefs ${PYRO_PFB_FS_KB}KB")
    math(EXPR _pyro_wdog_ms "2 * ${PYRO_LOOP_WORST_MS}")
    message(STATUS "  loop budget: ${PYRO_LOOP_WORST_MS}ms worst case, watchdog ${_pyro_wdog_ms}ms")
else()
    message(STATUS "Pyro target board: ${PYRO_BOARD} (${BOARD_DISPLAY_NAME}), kind=host "
                   "-- no firmware image is produced; build the 'sim' target")
endif()

# ── Extra flags a host board may ask for ─────────────────────────────
#
# A host board that models real hardware needs more than boards/sim does:
# the simulated-board packages (boards/sim_mk1a, sim_mk1b, sim_mk1c) compile
# the REAL board file from boards/<name>/pyro_board.c against the Pico SDK
# stand-in in sim/hw/ and the plant in sim/plant/, so they need those
# directories on the include path and two defines to select the wiring.
#
# A board declares them as PYRO_HOST_INCLUDE_DIRS and PYRO_HOST_DEFINES in
# its board.cmake; both default to empty, so boards/sim and every pico board
# are unaffected. Adding another modelled board needs no edit here.
set(PYRO_HOST_INC_FLAGS "")
foreach(_d ${PYRO_HOST_INCLUDE_DIRS})
    list(APPEND PYRO_HOST_INC_FLAGS -I${_d})
endforeach()
set(PYRO_HOST_DEF_FLAGS "")
foreach(_d ${PYRO_HOST_DEFINES})
    list(APPEND PYRO_HOST_DEF_FLAGS -D${_d})
endforeach()

# Pull in Raspberry Pi Pico SDK (must be before project)
if(PYRO_BOARD_KIND STREQUAL "pico")
    include(pico_sdk_import.cmake)
endif()

project(pyro_fw_c C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
if(PYRO_BOARD_KIND STREQUAL "pico")
    pico_sdk_init()
endif()

# ====================================================================================
include(FetchContent)

FetchContent_Declare(
    littlefs
    GIT_REPOSITORY https://github.com/littlefs-project/littlefs
    GIT_TAG v2.11.3
)

# Lua 5.4 for user programs on core1. Only fetched for boards that want it.
if(PYRO_HAS_LUA)
    FetchContent_Declare(
        lua
        GIT_REPOSITORY https://github.com/lua/lua.git
        GIT_TAG v5.4.6
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/src/lua/lua_patch.cmake
    )
endif()

FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.0
)

# A/B OTA bootloader
set(PFB_WITH_IMAGE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(PFB_WITH_SHA256_HASHING OFF CACHE BOOL "" FORCE)
set(PFB_REDIRECT_BOOTLOADER_LOGS_TO_UART ON CACHE BOOL "" FORCE)
set(PFB_RESERVED_FILESYSTEM_SIZE_KB ${PYRO_PFB_FS_KB} CACHE STRING "" FORCE)
FetchContent_Declare(
    pico_fota_bootloader
    GIT_REPOSITORY https://github.com/JZimnol/pico_fota_bootloader.git
    GIT_TAG 266cd6eb29205a6231afd9cf74bab915f063e707 # master, 2026-07-23
)

# FreeRTOS SMP [DD-073]. SOURCE_SUBDIR adds only the RP2040 port, which adds
# the kernel's libraries once the SDK is initialised.
if(PYRO_BOARD_KIND STREQUAL "pico")
    set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/src/rtos)
    FetchContent_Declare(
        freertos_kernel
        GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
        GIT_TAG V11.3.1
        SOURCE_SUBDIR portable/ThirdParty/GCC/RP2040
    )
endif()

if(PYRO_BOARD_KIND STREQUAL "pico")
    FetchContent_MakeAvailable(littlefs unity pico_fota_bootloader freertos_kernel)
else()
    FetchContent_MakeAvailable(littlefs unity) # no bootloader on a host build
endif()
if(PYRO_HAS_LUA)
    FetchContent_MakeAvailable(lua)
endif()

# ====================================================================================
# Lua 5.4 static library
#
# Upstream ships no CMakeLists, so the source set is listed here. Four
# libraries are deliberately excluded rather than sandboxed later:
#
#   lua.c, luac.c   standalone interpreter and compiler mains
#   loslib.c        os.execute, os.tmpname, os.exit -- no place on a flight computer
#   liolib.c        FILE* I/O; scripts reach the filesystem through our own API
#   loadlib.c       dynamic library loading
#   linit.c         its loadedlibs table would pull the above back in
#   lcorolib.c      coroutine table; pyro_lua.c never requiref's it, and the
#                   coroutine machinery ldo.c/lstate.c provides is separate
#   ldblib.c        debug table -- debug.sethook() would defeat the instruction
#                   budget outright, so its absence is load-bearing, not tidy
#   lutf8lib.c      utf8 table; never requiref'd
#
# Numbers are 32-bit float and 32-bit int, which halves every Lua value
# against a fixed arena and keeps the simulator numerically identical to
# the target.
# src/lua/lua_patch.cmake sets that in luaconf.h, the only place it can be
# set; pyro_luaconf.h refuses to build against an unpatched tree.
# ====================================================================================
if(PYRO_HAS_LUA)
    set(PYRO_LUA_SOURCES
        lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c
        lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c
        ltm.c lundump.c lvm.c lzio.c
        lauxlib.c lbaselib.c lmathlib.c lstrlib.c ltablib.c
    )
    list(TRANSFORM PYRO_LUA_SOURCES PREPEND ${lua_SOURCE_DIR}/)
    add_library(lua_lib STATIC ${PYRO_LUA_SOURCES})
    target_include_directories(lua_lib PUBLIC ${lua_SOURCE_DIR} ${CMAKE_CURRENT_LIST_DIR}/src/lua)
    target_compile_definitions(lua_lib PUBLIC LUA_USER_H="pyro_luaconf.h")
    target_compile_options(lua_lib PRIVATE -Wno-unused-but-set-variable -Wno-implicit-fallthrough)
    # The interpreter is built for size, not speed, while the flight code stays
    # at -O3. A work unit is bounded by a deadline the hook checks, so a slower
    # VM executes fewer Lua instructions in its time box and nothing else
    # changes -- whereas the ~14KB -O3 costs would not fit MK1B's 384KB slot.
    target_compile_options(lua_lib PRIVATE -Os)
    set(PYRO_LUA_INCLUDE ${lua_SOURCE_DIR})

    # VM host: arena, limits, sandbox and bindings. Board-independent; the
    # platform half lives in boards/<name>/lua_platform_*.c
    add_library(pyro_lua STATIC
        src/lua/pyro_lua.c
        src/lua/lua_arena.c
        src/lua/lua_check.c
        src/lua/lua_iface.c
        src/pad_claim.c
    )
    target_include_directories(pyro_lua PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/lua ${CMAKE_CURRENT_LIST_DIR}/src)
    target_link_libraries(pyro_lua PUBLIC lua_lib)
endif()
message("littlefs is here: ${littlefs_SOURCE_DIR}")

# ── Board identification in the bootloader ───────────────────────────
#
# picotool reads the image at XIP_BASE, which with pico_fota_bootloader is the
# BOOTLOADER -- the application sits in an A/B slot further up and its
# .binary_info is never scanned. Verified on hardware: an app whose strings are
# provably in flash still reports "Program Information: none". So the board
# identity has to live here, or `picotool info` says pico_fota_bootloader /
# pico_board=pico for every board alike.
#
# Added to the dependency's target rather than by patching its sources. The
# file emits binary_info data and no code, so it cannot change what the
# bootloader does.
if(PYRO_BOARD_KIND STREQUAL "pico" AND TARGET pico_fota_bootloader)
    target_sources(pico_fota_bootloader PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/bootloader_binary_info.c)
    target_include_directories(pico_fota_bootloader PRIVATE
        ${PYRO_BOARD_DIR} ${CMAKE_CURRENT_LIST_DIR}/src)
    pico_set_program_name(pico_fota_bootloader "${BOARD_DISPLAY_NAME} bootloader")
endif()


# The flight software: the same files in the firmware, every host suite and
# the simulators [HAL-04]. scripts/build_wasm.sh reads the same list.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/src/flight_sources.txt PYRO_FLIGHT_SOURCES)
set(PYRO_FLIGHT_SOURCES_ABS ${PYRO_FLIGHT_SOURCES})
list(TRANSFORM PYRO_FLIGHT_SOURCES_ABS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)

if(PYRO_BOARD_KIND STREQUAL "pico")
# ---- RP2040 firmware image ----------------------------------------
# Suppress warnings in third-party pico_fota_bootloader
get_target_property(_pfb_sources pico_fota_bootloader_lib SOURCES)
set_source_files_properties(${_pfb_sources} PROPERTIES COMPILE_OPTIONS "-Wno-array-bounds")

add_library(littlefs_lib STATIC
    ${littlefs_SOURCE_DIR}/lfs.c
    ${littlefs_SOURCE_DIR}/lfs_util.c
)
target_include_directories(littlefs_lib PUBLIC
  ${littlefs_SOURCE_DIR}
)
target_compile_options(littlefs_lib INTERFACE -Wno-unused-function -Wno-null-dereference)
target_link_libraries(littlefs_lib INTERFACE hardware_flash)

# Static buffers keep littlefs off the heap, whose lock pico_multicore (linked
# by the FreeRTOS port) makes a mutex; src/littlefs_driver.c supplies them.
# LFS_THREADSAFE: one mount shared by every task, locked per call.
target_compile_definitions(littlefs_lib PUBLIC LFS_NO_MALLOC LFS_THREADSAFE)

# ====================================================================================

set(LWIP_DIR ${PICO_SDK_PATH}/lib/lwip)
set(TINYUSB_DIR ${PICO_SDK_PATH}/lib/tinyusb)

# Generate version.h on every build
add_custom_target(generate_version ALL
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_version.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h
            ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating version.h"
)

# Board support and the board-independent HAL. Must come after pico_sdk_init().
add_subdirectory(src/hal_common)
add_subdirectory(${PYRO_BOARD_DIR} ${CMAKE_BINARY_DIR}/board)

# These two libraries are mutually dependent by design: hal_common implements
# hal.h by calling board_if.h, and the board's pyro/pressure code calls back
# into hal_common (hal_telemetry_send, etc). Declaring the cycle makes CMake
# repeat both archives on the link line; without it GNU ld's single pass
# leaves the board's symbols unresolved.
target_link_libraries(pyro_hal_common PUBLIC pyro_board)

add_executable(pyro_fw_c 
    src/main_hardware.c
    src/board_binary_info.c
    src/board_identity.c
    src/mac_random.c
    ${PYRO_FLIGHT_SOURCES}
    src/ground_test_switch.c
    src/buzzer.c
    src/ms5607_driver.c
    src/bmp280_driver.c
    src/pressure_collector.c
    src/i2c_recover.c
    src/usb_descriptors.c
    src/net_glue.c
    src/http_server.c
    src/http_conn.c
    src/http_work.c
    src/status_json.c
    src/net_stats.c
    src/net_txq.c
    src/flight_log.c
    src/log_plan.c
    src/pressure_trace.c
    src/net_ring.c
    src/reset_interface.c
    src/littlefs_driver.c
    src/flash_op.c
    src/vfs.c
    src/rtos/rtos_tasks.c
    src/net_task.c
    src/storage_task.c
    src/pin_caps.c
    src/pin_assign.c
    src/pin_store.c
    # TinyUSB networking helpers
    ${TINYUSB_DIR}/lib/networking/dhserver.c
    ${TINYUSB_DIR}/lib/networking/dnserver.c
    ${TINYUSB_DIR}/lib/networking/rndis_reports.c
    # lwIP core
    ${LWIP_DIR}/src/core/altcp.c
    ${LWIP_DIR}/src/core/altcp_alloc.c
    ${LWIP_DIR}/src/core/altcp_tcp.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
    ${LWIP_DIR}/src/core/ipv4/autoip.c
    ${LWIP_DIR}/src/core/ipv4/dhcp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/igmp.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/netif/ethernet.c
    # lwIP mDNS
    ${LWIP_DIR}/src/apps/mdns/mdns.c
    ${LWIP_DIR}/src/apps/mdns/mdns_out.c
    ${LWIP_DIR}/src/apps/mdns/mdns_domain.c
 )

 target_link_libraries(pyro_fw_c
        pyro_board        # boards/${PYRO_BOARD}: pins, pyro, pressure, board_if
        pyro_hal_common   # src/hal_common: hal.h implemented via board_if.h
        pico_stdlib
        pico_unique_id   # board_identity.c: per-board MAC and subnet
        hardware_i2c
        hardware_adc
        hardware_uart
        hardware_watchdog
        tinyusb_device
        tinyusb_board
        littlefs_lib
        pico_fota_bootloader_lib
        hardware_flash
        FreeRTOS-Kernel-Static
        )
target_include_directories(pyro_fw_c PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/rtos)

# The third-party stacks for size: FreeRTOS, lwIP and TinyUSB are not on the
# flight task's path, and at -O3 MK1B's 384 kB slot does not hold them.
file(GLOB_RECURSE _pyro_os_sources
    ${freertos_kernel_SOURCE_DIR}/*.c
    ${LWIP_DIR}/src/*.c
    ${TINYUSB_DIR}/src/*.c
    ${TINYUSB_DIR}/lib/networking/*.c)
set_source_files_properties(${_pyro_os_sources} PROPERTIES COMPILE_OPTIONS "-Os")

# So is the bench's side of the application: the HTTP server and what it
# parses and prints.
set_source_files_properties(
    src/http_server.c src/http_conn.c src/http_work.c src/status_json.c src/config.c src/pin_assign.c
    PROPERTIES COMPILE_OPTIONS "-Os")

# An SD card on SPI [DD-075, DD-076]: FatFs R0.16 (lib/fatfs) over this
# firmware's driver (src/sd).
if(PYRO_HAS_SD)
    target_sources(pyro_fw_c PRIVATE
        lib/fatfs/ff.c
        lib/fatfs/ffunicode.c
        src/sd/sd_card.c
        src/sd/spi_bus.c
        src/sd/diskio.c
        src/sd/ffsystem.c
        src/sd/lsm6ds3.c
        src/sd/hr_log.c
        src/sd/crc16.c)
    target_link_libraries(pyro_fw_c hardware_spi)
endif()

if(PYRO_HAS_LUA)
    target_sources(pyro_fw_c PRIVATE src/lua/lua_core1.c src/lua/lua_app.c)
    target_link_libraries(pyro_fw_c pyro_lua hardware_pio hardware_dma)
    target_compile_definitions(pyro_fw_c PRIVATE PYRO_HAS_LUA=1)
endif()

add_dependencies(pyro_fw_c generate_version)

pico_set_program_name(pyro_fw_c "${BOARD_DISPLAY_NAME}")
# The version is not set here: scripts/gen_version.sh writes src/version.h at
# build time, and src/board_binary_info.c emits FW_VERSION.

# Board identity comes from boards/<name>/board_pins.h via src/board_id.h,
# so no board name is passed on the command line.
target_compile_definitions(pyro_fw_c PRIVATE PYRO_FS_SIZE_KB=${PYRO_PFB_FS_KB})
# Main-loop budget; the watchdog timeout is derived from it. See the block in
# boards/<name>/board.cmake for what the number means and how to check it.
target_compile_definitions(pyro_fw_c PRIVATE PYRO_LOOP_WORST_MS=${PYRO_LOOP_WORST_MS})

# Flash geometry guard.
#
# pico_fota_bootloader computes its A/B slots as
#     swap = (PICO_FLASH_SIZE_BYTES - FS - 36k - 4k) / 2
# and asserts in the linker script that swap is a multiple of 4k. Those
# linker asserts do NOT catch an empty @PICO_FLASH_SIZE_BYTES@ substitution,
# which silently yields a negative flash size (see the note in each board's sdk/ header).
# Check it here, where the failure is legible.
math(EXPR _pyro_usable_kb "${PYRO_FLASH_SIZE_KB} - ${PYRO_PFB_FS_KB}")
math(EXPR _pyro_swap_kb   "(${_pyro_usable_kb} - 36 - 4) / 2")
math(EXPR _pyro_swap_rem  "(${_pyro_usable_kb} - 36 - 4) % 8")
math(EXPR _pyro_slot_kb   "${_pyro_swap_kb} - 128")
if(_pyro_usable_kb LESS_EQUAL 0)
    message(FATAL_ERROR "PYRO_PFB_FS_KB (${PYRO_PFB_FS_KB}) >= flash size (${PYRO_FLASH_SIZE_KB}KB)")
endif()
if(NOT _pyro_swap_rem EQUAL 0)
    message(FATAL_ERROR
        "Flash geometry not 4k-aligned: usable ${_pyro_usable_kb}KB gives a "
        "${_pyro_swap_kb}KB swap space. Adjust PYRO_PFB_FS_KB so that "
        "(flash - fs - 40) is a multiple of 8.")
endif()
if(_pyro_slot_kb LESS_EQUAL 0)
    message(FATAL_ERROR "No room for an application slot (${_pyro_slot_kb}KB)")
endif()
message(STATUS "  flash map: app slot ${_pyro_slot_kb}KB x2, littlefs ${PYRO_PFB_FS_KB}KB")

# Distinct artifact names so an MK1B image is never flashed to an MK1C board
set_target_properties(pyro_fw_c PROPERTIES OUTPUT_NAME "pyro_fw_${PYRO_BOARD}")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pyro_fw_c 0)
pico_enable_stdio_usb(pyro_fw_c 0)

# Add the standard include files to the build
target_include_directories(pyro_fw_c PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
        # src/lua/lua_platform_cfg.h holds the pin-role vocabulary, which
        # pin_assign.h needs. It pulls in no Lua headers, so a board built
        # without Lua can still see it.
        ${CMAKE_CURRENT_LIST_DIR}/src/lua
        ${LWIP_DIR}/src/include
        ${TINYUSB_DIR}/lib/networking
        ${PICO_SDK_PATH}/src/common/pico_usb_reset_interface_headers/include
)

target_include_directories(pyro_fw_c SYSTEM PUBLIC
        ${littlefs_SOURCE_DIR}
)

# Suppress warnings from lwIP and networking code
target_compile_options(pyro_fw_c PRIVATE
        -Wno-null-dereference
        -Wno-conversion
        -Wno-sign-conversion
        -Wno-sign-compare
)

pico_add_extra_outputs(pyro_fw_c)
pfb_compile_with_bootloader(pyro_fw_c)
endif() # PYRO_BOARD_KIND pico


# ====================================================================================
# Unit Tests (host-compiled, run on build machine)
# ====================================================================================
# These tests use mocks instead of real hardware.
# They compile with the host compiler via a custom command.

# Find host C compiler (not the ARM cross-compiler)
find_program(HOST_CC NAMES gcc cc PATHS /usr/bin /usr/local/bin NO_CMAKE_FIND_ROOT_PATH)

# What the mocked HAL links beside the flight software.
set(PYRO_TEST_HAL_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
    ${CMAKE_CURRENT_LIST_DIR}/src/flight_log.c
    ${CMAKE_CURRENT_LIST_DIR}/src/log_plan.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pyro_release.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pad_claim.c
)

# A suite of the flight software against the mocked HAL [HAL-05, TST-01]:
#   pyro_flight_suite(<target> <test.c> [more sources, relative to the tree])
function(pyro_flight_suite target test_source)
    set(extra "")
    foreach(source ${ARGN})
        list(APPEND extra ${CMAKE_CURRENT_LIST_DIR}/${source})
    endforeach()
    add_custom_target(${target}
        COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
                   -I${CMAKE_CURRENT_LIST_DIR}/test -I${CMAKE_CURRENT_LIST_DIR}/sim
                   -I${unity_SOURCE_DIR}/src
                   ${CMAKE_CURRENT_LIST_DIR}/${test_source}
                   ${extra}
                   ${PYRO_TEST_HAL_SOURCES}
                   ${PYRO_FLIGHT_SOURCES_ABS}
                   ${unity_SOURCE_DIR}/src/unity.c
                   -lm
                   -o ${CMAKE_BINARY_DIR}/test_${target}
        COMMAND ${CMAKE_BINARY_DIR}/test_${target}
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        COMMENT "${target}"
    )
endfunction()

# The board harness powers the flight software on and steps its main loop;
# flight_run flies it against a plant that says what really happened.
set(PYRO_HARNESS test/board_harness.c)
set(PYRO_FLOWN test/board_harness.c test/flight_run.c sim/mach_plant.c)

pyro_flight_suite(flight_boot_tests test/test_flight_boot.c ${PYRO_HARNESS})
pyro_flight_suite(flight_pad_tests test/test_flight_pad.c ${PYRO_FLOWN})
pyro_flight_suite(recorded_flight_tests test/test_recorded_flight.c ${PYRO_HARNESS})
pyro_flight_suite(flight_profile_tests test/test_flight_profiles.c ${PYRO_FLOWN} sim/replay.c)
pyro_flight_suite(fire_rule_tests test/test_fire_rules.c ${PYRO_FLOWN})
pyro_flight_suite(resume_tests test/test_resume.c ${PYRO_FLOWN})
pyro_flight_suite(estimator_tests test/test_estimator.c ${PYRO_FLOWN})
pyro_flight_suite(telemetry_tests test/test_telemetry.c ${PYRO_FLOWN})
pyro_flight_suite(mach_tests test/test_mach.c ${PYRO_FLOWN})
pyro_flight_suite(ground_test_tests test/test_ground_test.c ${PYRO_HARNESS})
pyro_flight_suite(buzzer_tests test/test_buzzer.c src/buzzer.c)
pyro_flight_suite(config_persistence_tests test/test_config_persistence.c src/buzzer.c)

# Pure modules, each through its own header.
function(pyro_module_suite target test_source)
    set(sources "")
    foreach(source ${ARGN})
        list(APPEND sources ${CMAKE_CURRENT_LIST_DIR}/${source})
    endforeach()
    add_custom_target(${target}
        COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src -I${unity_SOURCE_DIR}/src
                   ${CMAKE_CURRENT_LIST_DIR}/${test_source} ${sources}
                   ${unity_SOURCE_DIR}/src/unity.c -lm
                   -o ${CMAKE_BINARY_DIR}/test_${target}
        COMMAND ${CMAKE_BINARY_DIR}/test_${target}
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        COMMENT "${target}"
    )
endfunction()

pyro_module_suite(atmosphere_tests test/test_atmosphere.c src/atmosphere.c)
pyro_module_suite(launch_detector_tests test/test_launch_detector.c src/launch_detector.c src/atmosphere.c)
pyro_module_suite(apogee_detector_tests test/test_apogee_detector.c src/apogee_detector.c)
pyro_module_suite(estimators_tests test/test_estimators.c src/estimator_table.c src/estimator_lumped.c
                  src/estimator_constacc.c src/pressure_estimator.c src/atmosphere.c)
pyro_module_suite(fire_control_tests test/test_fire_control.c src/fire_control.c src/atmosphere.c)

# Plant tests. Each board model includes its own board's board_pins.h, so
# the three cannot share one include path -- they are compiled separately
# and linked together, which is also the only build that has all three
# models in one binary.
add_custom_target(plant_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building plant tests..."
    COMMAND ${HOST_CC} -c -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/sim/plant
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1a
               ${CMAKE_CURRENT_LIST_DIR}/sim/plant/plant_mk1a.c -o ${CMAKE_BINARY_DIR}/plant_mk1a.o
    COMMAND ${HOST_CC} -c -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/sim/plant
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1b
               ${CMAKE_CURRENT_LIST_DIR}/sim/plant/plant_mk1b.c -o ${CMAKE_BINARY_DIR}/plant_mk1b.o
    COMMAND ${HOST_CC} -c -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/sim/plant
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1c
               ${CMAKE_CURRENT_LIST_DIR}/sim/plant/plant_mk1c.c -o ${CMAKE_BINARY_DIR}/plant_mk1c.o
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/sim/plant
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_plant.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/plant/net_solve.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/plant/plant.c
               ${CMAKE_BINARY_DIR}/plant_mk1a.o
               ${CMAKE_BINARY_DIR}/plant_mk1b.o
               ${CMAKE_BINARY_DIR}/plant_mk1c.o
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_plant
    COMMAND ${CMAKE_COMMAND} -E echo "Running plant tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_plant
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Board plant model tests"
)

add_custom_target(sim
    COMMAND ${CMAKE_COMMAND} -E echo "Building simulator..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               ${PYRO_HOST_INC_FLAGS} ${PYRO_HOST_DEF_FLAGS}
               ${CMAKE_CURRENT_LIST_DIR}/sim/sim_cli.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/main_sim.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/replay.c
               ${PYRO_HOST_HAL_SOURCES}
               ${PYRO_FLIGHT_SOURCES_ABS}
               ${CMAKE_CURRENT_LIST_DIR}/src/pad_claim.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pyro_release.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               -lm
               -o ${CMAKE_BINARY_DIR}/pyro_sim
    COMMAND ${CMAKE_COMMAND} -E echo "Simulator built: ${CMAKE_BINARY_DIR}/pyro_sim"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Flight simulator"
)

# Lua sandbox and limit tests. Only when the selected board provides Lua.
if(PYRO_HAS_LUA)
# The sandbox tests always build against the SIMULATOR platform, whatever
# board is selected: they assert properties of the sandbox and the arena,
# which are board-independent, and pinning them to one platform keeps the
# assertions about resource names stable.
add_custom_target(lua_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building Lua tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/src/lua
               -I${lua_SOURCE_DIR}
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               "-DLUA_USER_H=\"pyro_luaconf.h\""
               # Counted by the tests that assert the VM never reaches them.
               -Dstrtof=counted_strtof -Dstrtod=counted_strtod
               -Dmalloc=counted_malloc -Drealloc=counted_realloc
               ${CMAKE_CURRENT_LIST_DIR}/test/test_lua.c
               ${CMAKE_CURRENT_LIST_DIR}/src/lua/pyro_lua.c
               ${CMAKE_CURRENT_LIST_DIR}/src/lua/lua_arena.c
               ${CMAKE_CURRENT_LIST_DIR}/src/lua/lua_check.c
               ${CMAKE_CURRENT_LIST_DIR}/src/lua/lua_iface.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pad_claim.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/sim/lua_platform_sim.c
               ${PYRO_LUA_SOURCES}
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_lua
    COMMAND ${CMAKE_COMMAND} -E echo "Running Lua tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Lua sandbox and resource-limit tests"
    VERBATIM
)
endif()

# Pin capability table. Built against MK1A's real table whatever board is
# selected: the structural invariants are _Static_assert and already fail the
# build, so what is left to test is the runtime surface, and pinning it to one
# board keeps the assertions about specific pins stable.
# The board self-test's decision table. Pure, so every case it can reach is
# checkable without a filesystem or a board -- including the two asymmetric
# ones that matter: a mismatch must refuse to commit, and no evidence must
# not refuse.
add_custom_target(board_selftest_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building board self-test tests..."
    COMMAND ${HOST_CC} -Wall -Wextra
               -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1c
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_board_selftest.c
               ${CMAKE_CURRENT_LIST_DIR}/src/board_selftest.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_board_selftest
    COMMAND ${CMAKE_COMMAND} -E echo "Running board self-test tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_board_selftest
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Board self-test decision tests"
)

add_custom_target(pin_caps_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building pin capability tests..."
    COMMAND ${HOST_CC} -Wall -Wextra
               -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1a
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_pin_caps.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pin_caps.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_pin_caps
    COMMAND ${CMAKE_COMMAND} -E echo "Running pin capability tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_pin_caps
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Pin capability table tests"
)

# The HTTP engine over a byte stream: no lwIP, no littlefs.
add_custom_target(http_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building HTTP stream tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_http.c
               ${CMAKE_CURRENT_LIST_DIR}/src/http_conn.c
               ${CMAKE_CURRENT_LIST_DIR}/src/net_ring.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_http
    COMMAND ${CMAKE_COMMAND} -E echo "Running HTTP stream tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_http
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "HTTP stream parser and framing tests"
)

# Who runs the HTTP work units: core0's turn and budget, and the worker's
# exclusive hold on the connections it takes (DD-061).
add_custom_target(http_work_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building HTTP work unit tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_http_work.c
               ${CMAKE_CURRENT_LIST_DIR}/src/http_work.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_http_work
    COMMAND ${CMAKE_COMMAND} -E echo "Running HTTP work unit tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_http_work
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "HTTP work unit scheduling tests"
)

# The flight log's binary records and the CSV they render as (DD-062).
add_custom_target(flight_log_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building flight log format tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_flight_log.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_log.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_flight_log
    COMMAND ${CMAKE_COMMAND} -E echo "Running flight log format tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_flight_log
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Flight log format tests"
)

# The three logging plans: what each keeps, in time order (DD-064).
add_custom_target(log_plan_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building logging plan tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_log_plan.c
               ${CMAKE_CURRENT_LIST_DIR}/src/log_plan.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_log_plan
    COMMAND ${CMAKE_COMMAND} -E echo "Running logging plan tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_log_plan
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Logging plan tests"
)

# The pressure trace's ring, as /api/pressure/trace reads it.
add_custom_target(pressure_trace_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building pressure trace tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_pressure_trace.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_trace.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_pressure_trace
    COMMAND ${CMAKE_COMMAND} -E echo "Running pressure trace tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_pressure_trace
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Pressure trace ring tests"
)

# /api/status from a snapshot, linked alone: the renderer reads nothing else.
# The USB network's transmit queue, against a fake endpoint (G4-N).
add_custom_target(net_txq_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building network transmit queue tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_net_txq.c
               ${CMAKE_CURRENT_LIST_DIR}/src/net_txq.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_net_txq
    COMMAND ${CMAKE_COMMAND} -E echo "Running network transmit queue tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_net_txq
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "USB network transmit queue tests"
)

# The high-rate log, its real reader and writer stepped by hand, on FatFs over
# a RAM disk [DD-077].
add_custom_target(hr_log_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building high-rate log tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -Wno-unused-parameter -DPYRO_HAS_SD=1
               -I${CMAKE_CURRENT_LIST_DIR}/test/fake_rtos
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1c_sd
               -I${CMAKE_CURRENT_LIST_DIR}/src/sd -I${CMAKE_CURRENT_LIST_DIR}/lib/fatfs
               -I${CMAKE_CURRENT_LIST_DIR}/src/rtos -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_hr_log.c
               ${CMAKE_CURRENT_LIST_DIR}/src/sd/hr_log.c
               ${CMAKE_CURRENT_LIST_DIR}/src/sd/crc16.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_trace.c
               ${CMAKE_CURRENT_LIST_DIR}/lib/fatfs/ff.c
               ${CMAKE_CURRENT_LIST_DIR}/lib/fatfs/ffunicode.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_hr_log
    COMMAND ${CMAKE_COMMAND} -E echo "Running high-rate log tests..."
    COMMAND ${CMAKE_COMMAND} -E env HR_LOG_SAMPLE=${CMAKE_BINARY_DIR}/hr_sample.bin ${CMAKE_BINARY_DIR}/test_hr_log
    COMMAND python3 support/hr_log.py --selftest
    COMMAND python3 support/hr_log.py --summary ${CMAKE_BINARY_DIR}/hr_sample.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "High-rate log tests"
)

# A MAC drawn from the RNG and the /serial.txt that keeps it [DD-072].
add_custom_target(mac_random_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building MAC draw tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_mac_random.c
               ${CMAKE_CURRENT_LIST_DIR}/src/mac_random.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_mac_random
    COMMAND ${CMAKE_COMMAND} -E echo "Running MAC draw tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_mac_random
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "MAC draw tests"
)

# The bench flight source: the atmosphere, the profile and the bench (DD-078).
add_custom_target(flight_sim_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building flight source tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_flight_sim.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_sim.c
               ${CMAKE_CURRENT_LIST_DIR}/src/bench_flight.c
               ${CMAKE_CURRENT_LIST_DIR}/src/atmosphere.c
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_flight_sim
    COMMAND ${CMAKE_COMMAND} -E echo "Running flight source tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_flight_sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Flight source tests"
)

# /api/net's rendering, the network's counters for the bench (G4-N).
add_custom_target(net_stats_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building network counter tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_net_stats.c
               ${CMAKE_CURRENT_LIST_DIR}/src/net_stats.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_net_stats
    COMMAND ${CMAKE_COMMAND} -E echo "Running network counter tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_net_stats
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "/api/net rendering tests"
)

add_custom_target(status_json_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building status rendering tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_status_json.c
               ${CMAKE_CURRENT_LIST_DIR}/src/status_json.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_status_json
    COMMAND ${CMAKE_COMMAND} -E echo "Running status rendering tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_status_json
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "/api/status rendering tests"
)

# The pressure collector's interrupt state machine, on a fake bus, clock and
# part: test/collector_bus.h stands in for src/hal_common/collector_bus.h, so
# -I test comes first.
add_custom_target(collector_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building pressure collector tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_collector.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_collector.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_collector
    COMMAND ${CMAKE_COMMAND} -E echo "Running pressure collector tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_collector
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Pressure collector state machine tests"
)

# The MS5607's arithmetic: compensation, the temperature's line, the PROM's CRC.
add_custom_target(ms5607_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building MS5607 tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_ms5607.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_ms5607
    COMMAND ${CMAKE_COMMAND} -E echo "Running MS5607 tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_ms5607
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "MS5607 arithmetic tests"
)

# A board's pyro backend run on the host against test/fake_sdk, whose sleeps
# fail the test (DD-053). Pinned to MK1B's board directory, whatever board
# this build is for.
add_custom_target(board_pyro_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building board pyro tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1b -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/src/lua -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_board_pyro_mk1b.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1b/pyro_board.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_board_pyro_mk1b
    COMMAND ${HOST_CC} -Wall -Wextra -DBOARD_PYRO_U5_DISCHARGES_OUTPUTS=0 -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1b -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/src/lua -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_board_pyro_mk1b.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1b/pyro_board.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_board_pyro_mk1b_base_part
    COMMAND ${CMAKE_COMMAND} -E echo "Running board pyro tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_board_pyro_mk1b
    COMMAND ${CMAKE_BINARY_DIR}/test_board_pyro_mk1b_base_part
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Board pyro backend tests"
)

add_custom_target(board_pyro_mk1a_tests
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1a -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_board_pyro_mk1a.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1a/pyro_board.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_board_pyro_mk1a
    COMMAND ${CMAKE_BINARY_DIR}/test_board_pyro_mk1a
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "MK1A pyro backend tests"
)
add_dependencies(board_pyro_tests board_pyro_mk1a_tests)

# Every board's pressure sensor bring-up, on the host against test/fake_sdk
# and fake sensors, each board built against its own package (DD-053).
add_custom_target(sensor_bringup_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building sensor bring-up tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1a -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_sensor_bringup.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_i2c.c
               ${CMAKE_CURRENT_LIST_DIR}/src/i2c_recover.c ${CMAKE_CURRENT_LIST_DIR}/src/ms5607_driver.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_single_sensor.c ${CMAKE_CURRENT_LIST_DIR}/src/bmp280_driver.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_sensor_bringup_mk1a
    COMMAND ${CMAKE_COMMAND} -E echo "Running sensor bring-up tests: mk1a"
    COMMAND ${CMAKE_BINARY_DIR}/test_sensor_bringup_mk1a
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1b -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_sensor_bringup.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_i2c.c
               ${CMAKE_CURRENT_LIST_DIR}/src/i2c_recover.c ${CMAKE_CURRENT_LIST_DIR}/src/ms5607_driver.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1b/pressure_board.c ${CMAKE_CURRENT_LIST_DIR}/src/bmp280_driver.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_sensor_bringup_mk1b
    COMMAND ${CMAKE_COMMAND} -E echo "Running sensor bring-up tests: mk1b"
    COMMAND ${CMAKE_BINARY_DIR}/test_sensor_bringup_mk1b
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1c -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_sensor_bringup.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_i2c.c
               ${CMAKE_CURRENT_LIST_DIR}/src/i2c_recover.c ${CMAKE_CURRENT_LIST_DIR}/src/ms5607_driver.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_single_sensor.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_sensor_bringup_mk1c
    COMMAND ${CMAKE_COMMAND} -E echo "Running sensor bring-up tests: mk1c"
    COMMAND ${CMAKE_BINARY_DIR}/test_sensor_bringup_mk1c
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/reference -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_sensor_bringup.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_i2c.c
               ${CMAKE_CURRENT_LIST_DIR}/src/i2c_recover.c ${CMAKE_CURRENT_LIST_DIR}/src/ms5607_driver.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_single_sensor.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_sensor_bringup_reference
    COMMAND ${CMAKE_COMMAND} -E echo "Running sensor bring-up tests: reference"
    COMMAND ${CMAKE_BINARY_DIR}/test_sensor_bringup_reference
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Pressure sensor bring-up tests, every board"
)

# The ground test procedure's schedule, against a fake clock (DD-071).
add_custom_target(ground_test_seq_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building ground test sequence tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_ground_test_seq.c
               ${CMAKE_CURRENT_LIST_DIR}/src/ground_test_seq.c
               ${CMAKE_CURRENT_LIST_DIR}/src/ground_test_switch.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_ground_test_seq
    COMMAND ${CMAKE_COMMAND} -E echo "Running ground test sequence tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_ground_test_seq
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Ground test sequence tests"
)

# The BMP280 at the loop's rate: one forced conversion a loop (DD-067), on a
# fake part timed from the datasheet, with MK1A's pins.
add_custom_target(bmp280_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building BMP280 tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1a -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_bmp280.c
               ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_sdk.c ${CMAKE_CURRENT_LIST_DIR}/test/fake_sdk/fake_i2c.c
               ${CMAKE_CURRENT_LIST_DIR}/src/bmp280_driver.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_bmp280
    COMMAND ${CMAKE_COMMAND} -E echo "Running BMP280 tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_bmp280
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "BMP280 forced-mode tests"
)

# MK1C's real pyro backend against sim/hw/'s SDK stand-in and the plant model
# of the board as measured: presence and shorts, and nothing else (DD-055).
add_custom_target(board_pyro_mk1c_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building MK1C pyro backend tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -DPYRO_SIM_BOARD_PYRO
               -I${CMAKE_CURRENT_LIST_DIR}/sim/hw -I${CMAKE_CURRENT_LIST_DIR}/sim/plant
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1c -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_board_pyro_mk1c.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1c/pyro_board.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1c/pyro_measure.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1c/pyro_faults.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1c/pyro_sequence.c
               ${CMAKE_CURRENT_LIST_DIR}/boards/mk1c/arm_pump.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/hw/rp2040_shim.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/plant/net_solve.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/plant/plant.c
               ${CMAKE_BINARY_DIR}/plant_mk1a.o
               ${CMAKE_BINARY_DIR}/plant_mk1b.o
               ${CMAKE_BINARY_DIR}/plant_mk1c.o
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_board_pyro_mk1c
    COMMAND ${CMAKE_COMMAND} -E echo "Running MK1C pyro backend tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_board_pyro_mk1c
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "MK1C pyro backend on the modelled board"
)
add_dependencies(board_pyro_mk1c_tests plant_tests)

# The beep vocabulary: the rules half, with no file I/O, like pin_assign.
add_custom_target(beep_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building beep code tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_beep_codes.c
               ${CMAKE_CURRENT_LIST_DIR}/src/beep_codes.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_beep_codes
    COMMAND ${CMAKE_COMMAND} -E echo "Running beep code tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_beep_codes
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Beep code vocabulary tests"
)

# The altitude beep-out, heard from the buzzer pin: buzzer.c alone on a HAL
# that records tone edges.
add_custom_target(beep_out_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building beep-out tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_beep_out.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_beep_out
    COMMAND ${CMAKE_COMMAND} -E echo "Running beep-out tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_beep_out
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Altitude beep-out tests"
)

# Pin assignment rules. Same reason as pin_caps_tests for pinning to MK1A:
# the release matrix needs a real table with two channels and a common.
add_custom_target(pin_assign_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building pin assignment tests..."
    COMMAND ${HOST_CC} -Wall -Wextra
               -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/src/lua
               -I${CMAKE_CURRENT_LIST_DIR}/boards/mk1a
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_pin_assign.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pin_assign.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pin_caps.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_pin_assign
    COMMAND ${CMAKE_COMMAND} -E echo "Running pin assignment tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_pin_assign
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Pin assignment rule tests"
)

add_custom_target(config_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building config tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_config.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_config
    COMMAND ${CMAKE_COMMAND} -E echo "Running config tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_config
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Config module tests (round-trip, parser, serializer)"
)

