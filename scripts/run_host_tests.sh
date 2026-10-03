#!/bin/bash
# Every host suite CI runs, against the local build directories. Prints one
# line per suite and exits 1 if any failed.
#
#   scripts/run_host_tests.sh            build (mk1b), build-mk1c, build-mk1a
#   VERBOSE=1 scripts/run_host_tests.sh  show each suite's output
cd "$(dirname "$0")/.." || exit 1

B=${B:-build} C=${C:-build-mk1c} A=${A:-build-mk1a}
fail=0

run() {
    local dir=$1 target=$2
    local out
    out=$(cmake --build "$dir" --target "$target" 2>&1)
    local rc=$?
    local summary
    summary=$(printf '%s\n' "$out" | grep -E '^[0-9]+ Tests [0-9]+ Failures' | tail -1)
    if [ $rc -ne 0 ]; then
        fail=1
        printf 'FAIL  %-28s %s  %s\n' "$target" "$dir" "$summary"
        printf '%s\n' "$out" | grep -E 'FAIL|error|Error' | head -20 | sed 's/^/      /'
    else
        printf 'ok    %-28s %s  %s\n' "$target" "$dir" "$summary"
    fi
    [ -n "$VERBOSE" ] && printf '%s\n' "$out"
}

# The flight software through the mocked HAL, then each module on its own.
for t in flight_boot_tests flight_pad_tests flight_profile_tests fire_rule_tests resume_tests \
         estimator_tests telemetry_tests mach_tests recorded_flight_tests ground_test_tests \
         ground_test_seq_tests atmosphere_tests fire_control_tests \
         http_tests http_work_tests status_json_tests net_stats_tests net_txq_tests mac_random_tests \
         hr_log_tests flight_sim_tests flight_log_tests pressure_trace_tests log_plan_tests \
         board_pyro_tests board_pyro_mk1c_tests plant_tests sensor_bringup_tests bmp280_tests \
         ms5607_tests board_selftest_tests pin_caps_tests beep_tests pin_assign_tests buzzer_tests \
         config_tests config_persistence_tests; do
    run "$B" "$t"
done
# The flight suites compile against the selected board's board_pins.h, which
# is where a board-specific assumption hides: start-up, the pad and a whole
# flight on the other boards too.
for d in "$C" "$A"; do
    for t in flight_boot_tests flight_pad_tests recorded_flight_tests; do
        run "$d" "$t"
    done
done
run "$C" lua_tests
run "$C" ms5607_tests

check() {
    local name=$1
    shift
    if "$@" >/dev/null 2>&1; then echo "ok    $name"; else echo "FAIL  $name"; fail=1; fi
}
check trace_check python3 support/trace_check.py --counts
check trace_matrix python3 support/trace_matrix.py --check
check structure_check python3 support/structure_check.py
check wait_check python3 support/wait_check.py
exit $fail
