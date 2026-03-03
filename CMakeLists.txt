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

FetchContent_MakeAvailable(littlefs unity)
message("littlefs is here: ${littlefs_SOURCE_DIR}")

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

set(LWIP_DIR ${USERHOME}/.pico-sdk/sdk/${sdkVersion}/lib/lwip)
set(TINYUSB_DIR ${USERHOME}/.pico-sdk/sdk/${sdkVersion}/lib/tinyusb)

add_executable(pyro_fw_c 
    src/flight_controller.c
    src/pressure_sensor.c
    src/pyro.c
    src/ms5607_driver.c
    src/bmp280_driver.c
    src/usb_descriptors.c
    src/net_glue.c
    src/http_server.c
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
 )

 target_link_libraries(pyro_fw_c
        pico_stdlib
        hardware_i2c
        hardware_adc
        hardware_uart
        tinyusb_device
        tinyusb_board
        littlefs_lib
        )

pico_set_program_name(pyro_fw_c "pyro_fw_c")
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

# ====================================================================================
# Unit Tests
# ====================================================================================

enable_testing()

# Test executable
add_executable(test_flight_controller
    test/test_flight_controller.c
    src/pressure_sensor.c
    src/ms5607_driver.c
    src/bmp280_driver.c
)

target_include_directories(test_flight_controller PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_flight_controller
    unity
    pico_stdlib
    hardware_i2c
    hardware_adc
    hardware_uart
)

add_test(NAME flight_controller_tests COMMAND test_flight_controller)

