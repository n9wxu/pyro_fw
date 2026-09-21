# Intentionally empty.
#
# A host-kind board contributes sources through PYRO_HOST_HAL_SOURCES in
# board.cmake rather than a `pyro_board` library target, because the host
# and WASM builds are custom targets driven by HOST_CC and emcc, not by the
# Pico SDK toolchain. The top level does not add_subdirectory() a host board.
