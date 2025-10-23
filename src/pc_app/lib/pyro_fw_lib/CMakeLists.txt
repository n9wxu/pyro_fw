cmake_minimum_required(VERSION 3.10)

add_library(pyro_lib pyro_lib.c)
target_include_directories(pyro_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

