# CMake generated Testfile for 
# Source directory: /Users/jjulich/pyro_fw
# Build directory: /Users/jjulich/pyro_fw/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(flight_controller_tests "/Users/jjulich/pyro_fw/build/test_flight_controller.elf")
set_tests_properties(flight_controller_tests PROPERTIES  _BACKTRACE_TRIPLES "/Users/jjulich/pyro_fw/CMakeLists.txt;135;add_test;/Users/jjulich/pyro_fw/CMakeLists.txt;0;")
subdirs("pico-sdk")
subdirs("_deps/unity-build")
