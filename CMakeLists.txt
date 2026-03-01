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

add_executable(pyro_fw_c 
    flight_controller.c
    pressure_sensor.c
    ms5607_driver.c
    bmp280_driver.c
 )

 target_link_libraries(pyro_fw_c
        pico_stdlib
        hardware_i2c
        hardware_adc
        hardware_uart
        )

pico_set_program_name(pyro_fw_c "pyro_fw_c")
pico_set_program_version(pyro_fw_c "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pyro_fw_c 0)
pico_enable_stdio_usb(pyro_fw_c 1)

# Add the standard include files to the build
target_include_directories(pyro_fw_c PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(pyro_fw_c)

