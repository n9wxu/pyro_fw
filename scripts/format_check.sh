#!/bin/bash
# clang-format over the files held to .clang-format. A file that is not
# formatted, or a listed file that does not exist, fails.
#
#   scripts/format_check.sh         check
#   scripts/format_check.sh --fix   reformat in place
cd "$(dirname "$0")/.." || exit 1

FILES="$(cat src/flight_sources.txt) src/flight_internal.h src/flight_states.h src/buzzer.c
    src/hal.h src/main_hardware.c
    src/board_if.h src/board_id.h
    src/hal_common/hal_common.c src/hal_common/board_defaults.c
    src/board_support.h src/pressure_single_sensor.c src/bmp280_driver.c src/bmp280_driver.h
    src/pressure_collector.c src/pressure_collector.h src/hal_common/collector_bus.h
    src/ms5607_driver.c src/ms5607_driver.h
    $(ls boards/*/board_info.c boards/*/pyro_*.c boards/*/pyro_*.h) boards/mk1b/pressure_board.c
    boards/mk1c/arm_pump.c boards/mk1c/arm_pump.h
    src/http_server.c src/http_server.h src/http_conn.c src/http_conn.h
    src/http_work.c src/http_work.h src/status_json.c src/status_json.h
    src/flight_log.c src/flight_log.h src/pressure_trace.c src/pressure_trace.h
    src/log_plan.c src/log_plan.h
    sim/physics.c sim/physics.h sim/sim_cli.c sim/pyro_sim.h test/test_physics.c"

if [ "$1" = "--fix" ]; then
    # shellcheck disable=SC2086
    exec clang-format -i $FILES
fi
# shellcheck disable=SC2086
exec clang-format --dry-run --Werror $FILES
