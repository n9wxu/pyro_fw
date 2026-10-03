#!/usr/bin/env python3
"""Requirements that are properties of the source, checked from the source.

    support/structure_check.py     exit 1 on any violation

Each rule names the requirement it verifies. The flight software is the list
in src/flight_sources.txt and the headers those files include from src/.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def flight_sources():
    return [line.strip() for line in read("src/flight_sources.txt").splitlines() if line.strip()]


def flight_headers(sources):
    seen = set()
    for src in sources:
        for name in re.findall(r'#include "([a-z_0-9]+\.h)"', read(src)):
            path = "src/" + name
            if os.path.exists(os.path.join(ROOT, path)):
                seen.add(path)
    return sorted(seen)


def find(files, pattern, flags=0):
    hits = []
    rx = re.compile(pattern, flags)
    for path in files:
        for n, line in enumerate(strip_comments(read(path)).splitlines(), 1):
            if rx.search(line):
                hits.append(f"{path}:{n}: {line.strip()[:90]}")
    return hits


def main():
    sources = flight_sources()
    flight = sources + flight_headers(sources)
    failures = []

    def rule(req, what, hits):
        for h in hits:
            failures.append(f"[{req}] {what}\n    {h}")

    rule("HAL-01", "flight software holds platform code or conditional compilation",
         find(flight, r"#\s*(if|ifdef|elif)\b|pico/|hardware/|FreeRTOS|\btask\.h\b"))
    rule("HAL-02", "flight software reaches hardware other than through hal.h",
         find(sources, r"\b(gpio_|adc_|i2c_|spi_|uart_|pio_|watchdog_|flash_range_|lfs_)\w*\s*\("))
    rule("TEL-12", "the telemetry port has a receive path",
         find(["src/hal.h"], r"hal_(serial|telemetry)_(read|recv|readline|getc|available)"))
    rule("CFG-10", "the running system reloads its configuration",
         find(flight, r"config_reload|pins_reload|flight_apply_config"))
    rule("CODE-09", "flight software has a test-only construct",
         find(flight, r"\b(UNIT_TEST|HOST_TEST|TESTING|TEST_BUILD|PYRO_TEST)\b|_for_test\b|\btest_only\b"))
    rule("SYS-LUA-02", "flight software waits on, or calls into, the script",
         find(flight, r"\blua_\w+\s*\("))
    rule("LUA-ISO-03", "the script interface can fire a channel or stop one firing",
         find(["src/lua/pyro_lua.c", "src/lua/lua_app.c"], r"hal_pyro_fire|hal_pyro_abort|pyro_fire\s*\("))
    rule("SNS-REC-01", "flight software re-initialises or resets the sensor",
         [h for h in find(sources, r"hal_pressure_(init|reset|recover)\s*\(") if "flight_states.c" not in h])
    rule("PYR-FIRE-01", "a fire is refused",
         find(sources + ["boards/mk1a/pyro_board.c", "boards/mk1b/pyro_board.c", "boards/mk1c/pyro_board.c",
                         "boards/mk1c/pyro_sequence.c"], r"REFUSED|_refusal\b|refuse_fire"))
    rule("SNS-ALT-01", "an altitude is clamped",
         find(flight, r"ALT(ITUDE)?_(MAX|CEILING|CLAMP)|8000\.0f\s*\)|MAX_ALTITUDE_M\b"))

    if failures:
        print("\n".join(failures))
        print(f"\n{len(failures)} violation(s)")
        return 1
    print("structure_check: every rule holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
