#!/usr/bin/env python3
"""
prove_flight_ram.py -- what the flight path fetches and reads from flash.

From launch to landing, outside test mode, the flight software -- the flight
task, the interrupt handlers that run then, and the flight log's writer --
is to fetch no instruction and read no data from the boot flash. That is a
property of the linked image, so this works on the ELF, with prove_core0.py's
disassembly: from each root it follows every direct branch, and reports each
function the closure reaches in flash, each indirect branch it cannot follow,
and each flash address a function loads.

Usage:
    support/prove_flight_ram.py build-alfs-mk1c/pyro_fw_mk1c.elf           report
    support/prove_flight_ram.py --list build-alfs-mk1c/pyro_fw_mk1c.elf    and every function, by size
    support/prove_flight_ram.py --strict build-alfs-mk1c/pyro_fw_mk1c.elf  fail on any finding

Functions are keyed by address: two static functions may share a name (the
SDK's panic() and Lua's), and merging them by name invents paths.

LIMITS. The roots are the states from launch to landing, but a function a
root calls is followed whatever branch of it runs in flight, so the closure
over-counts what flight runs: a function's pad or test-mode branch is in it.
A call through a function pointer is not followed.
"""
import argparse
import collections
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prove_core0 as pc  # noqa: E402

# From launch to landing: the flight task's loop and the states it passes
# through, the HAL tasks it ticks, the pyro and the log, the storage task's
# pass, and the interrupt handlers.
IN_FLIGHT_ROOTS = [
    "flight_task", "dispatch_state", "flight_update_outputs", "flight_call_service", "watchdog_update",
    "flight_action_launch", "flight_action_armed", "flight_action_apogee", "flight_action_landing",
    "flight_action_resumed_descent", "flight_detect_ascent", "flight_detect_falling",
    "flight_detect_drogue_descent", "flight_detect_chute_descent", "flight_detect_landed",
    "hal_tasks_tick", "pres_tick", "buzzer_tick",
    "hal_pyro_update", "pyro_update", "hal_pyro_fire", "hal_pyro_sample",
    "hal_log_start", "hal_log_sample", "hal_storage_service",
    "period_isr", "telem_uart_irq_handler", "collector_alarm_isr", "pulse_end_isr",
]

# Paths that end the program: followed no further.
LEAVES = {"panic", "__assert_func", "hard_assertion_failure", "panic_unsupported"}

ROM_HI = 0x00004000


def load(elf, objdump):
    out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True, check=True).stdout
    fns, by_name, cur = {}, collections.defaultdict(list), None
    for line in out.splitlines():
        m = pc.FN_ADDR_RE.match(line)
        if m:
            cur = int(m.group(1), 16)
            fns[cur] = (m.group(2), [])
            by_name[m.group(2)].append(cur)
        elif cur is not None and line.strip():
            fns[cur][1].append(line)
    sizes = {}
    syms = subprocess.run([objdump, "-t", elf], capture_output=True, text=True, check=True).stdout
    for line in syms.splitlines():
        m = pc.SYM_FUNC_RE.match(line)
        if m:
            sizes[int(m.group(1), 16) & ~1] = int(m.group(2), 16)
    return fns, by_name, sizes


def closure(fns, by_name, roots):
    """Every function the roots reach by direct branch, and what each does that this cannot clear."""
    seen, todo = set(), [a for r in roots for a in by_name.get(r, [])]
    indirect, loads = [], []
    while todo:
        a = todo.pop()
        if a in seen:
            continue
        seen.add(a)
        name, lines = fns[a]
        if name in LEAVES or a < ROM_HI:
            continue
        for line in lines:
            m = pc.BRANCH_RE.search(line)
            if m:
                target = int(m.group(2), 16)
                tname = m.group(3).split("+")[0]
                if tname.startswith("__") and tname.endswith("_veneer"):
                    landed = by_name.get(tname[2:-len("_veneer")])
                    target = landed[0] if landed else target
                elif "+" in m.group(3):
                    continue  # a branch inside a function
                if target in fns and target != a:
                    todo.append(target)
                continue
            if pc.INDIRECT_RE.search(line):
                indirect.append((name, line.strip()))
                continue
            m = pc.WORD_RE.search(line)
            if m and pc.XIP_LO <= int(m.group(1), 16) < pc.XIP_HI:
                loads.append((name, m.group(1)))
    return seen, indirect, loads


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    ap.add_argument("--list", action="store_true", help="every function in the closure, by size")
    a = ap.parse_args()
    fns, by_name, sizes = load(a.elf, pc.find_objdump())
    roots = [r for r in IN_FLIGHT_ROOTS if r in by_name]
    missing = [r for r in IN_FLIGHT_ROOTS if r not in by_name and r != "pulse_end_isr"]
    seen, indirect, loads = closure(fns, by_name, roots)
    code = [x for x in seen if x >= ROM_HI]
    flash = [x for x in code if not (pc.RAM_LO <= x < pc.RAM_HI)]
    total, flash_bytes = sum(sizes.get(x, 0) for x in code), sum(sizes.get(x, 0) for x in flash)
    print(f"=== {a.elf} ===")
    print(f"closure: {len(code)} functions, {total} bytes of code; {len(flash)} in flash, {flash_bytes} bytes")
    print(f"indirect branches: {len(indirect)} in {len(set(f for f, _ in indirect))} functions; "
          f"flash addresses loaded: {len(loads)} in {len(set(f for f, _ in loads))} functions")
    if missing:
        print(f"roots not in the image: {', '.join(missing)}")
    if a.list:
        for x in sorted(code, key=lambda x: -sizes.get(x, 0)):
            where = "RAM  " if pc.RAM_LO <= x < pc.RAM_HI else "flash"
            print(f"  {sizes.get(x, 0):6d} {where} {fns[x][0]}")
        for f, line in indirect:
            print(f"  indirect: {f}: {line}")
        for f, addr in loads:
            print(f"  load: {f}: 0x{addr}")
    if a.strict and (flash or indirect or loads or missing):
        print("FAIL  the flight path reaches flash")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
