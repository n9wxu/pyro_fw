# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/jjulich/pyro_fw/build/_deps/littlefs-src")
  file(MAKE_DIRECTORY "/Users/jjulich/pyro_fw/build/_deps/littlefs-src")
endif()
file(MAKE_DIRECTORY
  "/Users/jjulich/pyro_fw/build/_deps/littlefs-build"
  "/Users/jjulich/pyro_fw/build/_deps/littlefs-subbuild/littlefs-populate-prefix"
  "/Users/jjulich/pyro_fw/build/_deps/littlefs-subbuild/littlefs-populate-prefix/tmp"
  "/Users/jjulich/pyro_fw/build/_deps/littlefs-subbuild/littlefs-populate-prefix/src/littlefs-populate-stamp"
  "/Users/jjulich/pyro_fw/build/_deps/littlefs-subbuild/littlefs-populate-prefix/src"
  "/Users/jjulich/pyro_fw/build/_deps/littlefs-subbuild/littlefs-populate-prefix/src/littlefs-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/jjulich/pyro_fw/build/_deps/littlefs-subbuild/littlefs-populate-prefix/src/littlefs-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/jjulich/pyro_fw/build/_deps/littlefs-subbuild/littlefs-populate-prefix/src/littlefs-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
