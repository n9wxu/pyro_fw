cmake_minimum_required(VERSION 3.10)

project(pyro_test VERSION 1.0)

add_executable(pyro_test main.c)

target_include_directories(pyro_test PUBLIC
                           "${PROJECT_BINARY_DIR}"
                           "${PROJECT_SOURCE_DIR}/lib/pyro_fw_lib"
                           )

configure_file(pyro_testConfig.h.in pyro_testConfig.h)

add_subdirectory( lib/pyro_fw_lib )

target_link_libraries(pyro_test PUBLIC pyro_lib)