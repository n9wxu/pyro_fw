# ── Simulated Pyro MK1C ────────────────────────────────────────────────
#
# A host board that runs the REAL MK1C pyro backend against a model of the
# MK1C board. boards/sim runs the flight software with a pyro fixture, where
# continuity is whatever a test last wrote; this runs it with
# boards/mk1c/pyro_board.c driving sim/plant/plant_mk1c.c through the Pico SDK
# stand-in in sim/hw/.
#
# Use boards/sim when the question is about flight logic, and this when the
# question is about the board: sense thresholds, settle times, fault
# detection, or what the firmware does when an element fails.
#
#     cmake -B build-sim-mk1c -DPYRO_BOARD=sim_mk1c && cmake --build build-sim-mk1c --target sim

set(PYRO_BOARD_KIND host)
set(PYRO_HAS_LUA 0)   # the pyro model is the point here; Lua is boards/sim's job
set(BOARD_DISPLAY_NAME "Pyro MK1C (simulated board)")

set(_sim_root ${CMAKE_CURRENT_LIST_DIR}/../..)

# Consumed by the host targets in the top-level CMakeLists.
set(PYRO_HOST_INCLUDE_DIRS
    ${_sim_root}/boards/sim      # hal_sim.h, which sim_cli.c and main_sim.c use
    ${_sim_root}/sim/hw
    ${_sim_root}/sim/plant
    ${_sim_root}/boards/mk1c
)
set(PYRO_HOST_DEFINES
    PYRO_SIM_BOARD_PYRO      # hal_sim.c leaves hal_pyro_* to the glue
    PYRO_SIM_PLANT_MK1C      # which plant the glue instantiates
)

set(PYRO_HOST_HAL_SOURCES
    ${_sim_root}/boards/sim/hal_sim.c
    ${_sim_root}/boards/mk1c/pyro_board.c
    ${_sim_root}/sim/hw/rp2040_shim.c
    ${_sim_root}/sim/hw/pyro_sim_glue.c
    ${_sim_root}/sim/plant/net_solve.c
    ${_sim_root}/sim/plant/plant.c
    ${_sim_root}/sim/plant/plant_mk1c.c
)
