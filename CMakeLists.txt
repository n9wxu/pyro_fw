# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

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
#                     PICO_BOARD_KIND, BOARD_DISPLAY_NAME, PICO_BOARD,
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
    list(APPEND _required PYRO_FLASH_SIZE_KB PYRO_PFB_FS_KB)
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
else()
    message(STATUS "Pyro target board: ${PYRO_BOARD} (${BOARD_DISPLAY_NAME}), kind=host "
                   "-- no firmware image is produced; build the 'sim' target")
endif()

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
    GIT_TAG v2.11.2
)

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
    GIT_TAG master
)

if(PYRO_BOARD_KIND STREQUAL "pico")
    FetchContent_MakeAvailable(littlefs unity pico_fota_bootloader)
else()
    FetchContent_MakeAvailable(littlefs unity) # no bootloader on a host build
endif()
message("littlefs is here: ${littlefs_SOURCE_DIR}")

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
    src/flight_states.c
    src/pressure_processing.c
    src/ground_test.c
    src/config.c
    src/telemetry_formatter.c
    src/buzzer.c
    src/ms5607_driver.c
    src/usb_descriptors.c
    src/net_glue.c
    src/http_server.c
    src/reset_interface.c
    src/littlefs_driver.c
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
        hardware_i2c
        hardware_adc
        hardware_uart
        hardware_watchdog
        tinyusb_device
        tinyusb_board
        littlefs_lib
        pico_fota_bootloader_lib
        )

add_dependencies(pyro_fw_c generate_version)

pico_set_program_name(pyro_fw_c "${BOARD_DISPLAY_NAME}")
pico_set_program_version(pyro_fw_c "0.1")

# Board identity comes from boards/<name>/board_pins.h via src/board_id.h,
# so no board name is passed on the command line.
target_compile_definitions(pyro_fw_c PRIVATE PYRO_FS_SIZE_KB=${PYRO_PFB_FS_KB})

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

add_custom_target(host_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building host tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_processing.c
               ${CMAKE_CURRENT_LIST_DIR}/src/ground_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry_formatter.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_flight_states
    COMMAND ${CMAKE_COMMAND} -E echo "Running tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_flight_states
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Host unit tests"
)

add_custom_target(integration_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building integration tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_integration.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_processing.c
               ${CMAKE_CURRENT_LIST_DIR}/src/ground_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry_formatter.c
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_integration
    COMMAND ${CMAKE_COMMAND} -E echo "Running integration tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_integration
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Integration tests with OpenRocket data"
)

add_custom_target(closedloop_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building closed-loop tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_closedloop.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_processing.c
               ${CMAKE_CURRENT_LIST_DIR}/src/ground_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry_formatter.c
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_closedloop
    COMMAND ${CMAKE_COMMAND} -E echo "Running closed-loop tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_closedloop
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Closed-loop simulation tests"
)

add_custom_target(sim
    COMMAND ${CMAKE_COMMAND} -E echo "Building simulator..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/sim/sim_cli.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/main_sim.c
               ${PYRO_HOST_HAL_SOURCES}
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_processing.c
               ${CMAKE_CURRENT_LIST_DIR}/src/ground_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry_formatter.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               -lm
               -o ${CMAKE_BINARY_DIR}/pyro_sim
    COMMAND ${CMAKE_COMMAND} -E echo "Simulator built: ${CMAKE_BINARY_DIR}/pyro_sim"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Flight simulator"
)

add_custom_target(buzzer_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building buzzer pattern tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_buzzer.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_processing.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_buzzer
    COMMAND ${CMAKE_COMMAND} -E echo "Running buzzer tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_buzzer
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Buzzer pattern player unit tests"
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

add_custom_target(config_persistence_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building config persistence tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${PYRO_BOARD_DIR} -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_config_persistence.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/pressure_processing.c
               ${CMAKE_CURRENT_LIST_DIR}/src/ground_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/config.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry_formatter.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               ${unity_SOURCE_DIR}/src/unity.c
               -lm
               -o ${CMAKE_BINARY_DIR}/test_config_persistence
    COMMAND ${CMAKE_COMMAND} -E echo "Running config persistence tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_config_persistence
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Config persistence and runtime reload tests"
)

