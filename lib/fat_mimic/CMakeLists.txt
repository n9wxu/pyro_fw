cmake_minimum_required(VERSION 3.13)

project(fat_mimic C)

add_library(fat_mimic STATIC
    src/fat_mimic.c
)

target_include_directories(fat_mimic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Dependencies provided by parent project:
#   littlefs_lib, tinyusb_device, pico_stdlib
