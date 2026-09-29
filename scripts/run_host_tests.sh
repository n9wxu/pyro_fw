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

for t in host_tests closedloop_tests pressure_chain_tests mach_tests brownout_tests \
         http_tests http_work_tests status_json_tests net_stats_tests net_txq_tests mac_random_tests hr_log_tests \
         flight_log_tests pressure_trace_tests log_plan_tests board_pyro_tests \
         sensor_bringup_tests bmp280_tests ground_test_seq_tests ground_test_tests \
         pin_caps_tests beep_tests pin_assign_tests buzzer_tests config_tests \
         config_persistence_tests plant_tests board_pyro_mk1c_tests \
         integration_tests ms5607_tests; do
    run "$B" "$t"
done
run "$C" lua_tests
run "$C" integration_tests
run "$C" ms5607_tests
run "$A" integration_tests

python3 support/trace_check.py --counts >/dev/null 2>&1 && echo "ok    trace_check" || { echo "FAIL  trace_check"; fail=1; }
python3 support/wait_check.py >/dev/null 2>&1 && echo "ok    wait_check" || { echo "FAIL  wait_check"; fail=1; }
exit $fail
