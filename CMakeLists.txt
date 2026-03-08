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

set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pyro_fw_c C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

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
set(PFB_RESERVED_FILESYSTEM_SIZE_KB 984 CACHE STRING "" FORCE)
FetchContent_Declare(
    pico_fota_bootloader
    GIT_REPOSITORY https://github.com/JZimnol/pico_fota_bootloader.git
    GIT_TAG master
)

FetchContent_MakeAvailable(littlefs unity pico_fota_bootloader)
message("littlefs is here: ${littlefs_SOURCE_DIR}")

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

add_executable(pyro_fw_c 
    src/main_hardware.c
    src/hal_hardware.c
    src/flight_states.c
    src/telemetry.c
    src/buzzer.c
    src/pressure_sensor.c
    src/pyro.c
    src/ms5607_driver.c
    src/bmp280_driver.c
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

pico_set_program_name(pyro_fw_c "Pyro MK1B")
pico_set_program_version(pyro_fw_c "0.1")

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

# ====================================================================================
# Unit Tests (host-compiled, run on build machine)
# ====================================================================================
# These tests use mocks instead of real hardware.
# They compile with the host compiler via a custom command.

# Find host C compiler (not the ARM cross-compiler)
find_program(HOST_CC NAMES gcc cc PATHS /usr/bin /usr/local/bin NO_CMAKE_FIND_ROOT_PATH)

add_custom_target(host_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building host tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               ${unity_SOURCE_DIR}/src/unity.c
               -o ${CMAKE_BINARY_DIR}/test_flight_states
    COMMAND ${CMAKE_COMMAND} -E echo "Running tests..."
    COMMAND ${CMAKE_BINARY_DIR}/test_flight_states
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Host unit tests"
)

add_custom_target(integration_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Building integration tests..."
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_integration.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry.c
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
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               -I${CMAKE_CURRENT_LIST_DIR}/test
               -I${unity_SOURCE_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/test/test_closedloop.c
               ${CMAKE_CURRENT_LIST_DIR}/test/hal_test.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry.c
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
    COMMAND ${HOST_CC} -Wall -Wextra -I${CMAKE_CURRENT_LIST_DIR}/src
               ${CMAKE_CURRENT_LIST_DIR}/sim/sim_cli.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/main_sim.c
               ${CMAKE_CURRENT_LIST_DIR}/sim/hal_sim.c
               ${CMAKE_CURRENT_LIST_DIR}/src/flight_states.c
               ${CMAKE_CURRENT_LIST_DIR}/src/telemetry.c
               ${CMAKE_CURRENT_LIST_DIR}/src/buzzer.c
               -lm
               -o ${CMAKE_BINARY_DIR}/pyro_sim
    COMMAND ${CMAKE_COMMAND} -E echo "Simulator built: ${CMAKE_BINARY_DIR}/pyro_sim"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Flight simulator"
)

