cmake_minimum_required(VERSION 4.1)

include(FetchContent)

project(pyro_test VERSION 1.0)

FetchContent_Declare(
    inih_repo
    GIT_REPOSITORY https://github.com/OSSystems/inih.git
    GIT_TAG origin/master
)

FetchContent_Declare(
    unity_repo
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG origin/master
)
set(BUILD_SHARED_LIBS)

FetchContent_MakeAvailable(inih_repo unity_repo)

#add_subdirectory(${inih_SOURCE_DIR}/include)
#add_subdirectory(${inih_SOURCE_DIR}/lib)

add_executable(pyro_test main.c)

target_include_directories(pyro_test PUBLIC
                           "${PROJECT_BINARY_DIR}"
                           "${PROJECT_SOURCE_DIR}"
                           )


configure_file(pyro_testConfig.h.in pyro_testConfig.h)

add_subdirectory( lib/pyro_fw_lib )

target_link_libraries(pyro_test PUBLIC pyro_lib inih unity)

