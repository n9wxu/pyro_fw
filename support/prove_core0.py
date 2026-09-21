#!/usr/bin/env python3
"""
prove_core0.py -- prove (or disprove) that core1 can hang core0.

The safety rule for running Lua on core1 is a single sentence:

    core0 must never wait on anything core1 can hold.

That is a property of the linked binary, not of the source, because the
dangerous waits are introduced by the SDK underneath us. The clearest example:
linking pico_multicore defines LIB_PICO_MULTICORE, which makes
pico/malloc.h set PICO_USE_MALLOC_MUTEX=1, which rewrites every
malloc/calloc/realloc/free on BOTH cores to call

    mutex_enter_blocking(&malloc_mutex)

a function with no timeout parameter, no failure return, and a `do { }
while (true)` that exits only on acquiring the lock (pico-sdk 2.2.0,
src/common/pico_sync/mutex.c). Nothing in our source changed; a link line did.

So this tool works on the ELF. It reconstructs the call graph from the
disassembly, resolves long-branch veneers, and reports every path from a
flight-critical entry point to a primitive that can wait forever.

Usage:
    support/prove_core0.py build-mk1c/pyro_fw_mk1c.elf
    support/prove_core0.py --compare build-mk1c/...elf build-core1/...elf

Exit status is 1 if any flight-critical root can reach an unbounded wait,
so this runs in CI as the standing guard on the rule above.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

# ---------------------------------------------------------------------------
# What counts as "can wait forever"
#
# Each of these either has no timeout in its signature, or takes a deadline of
# at_the_end_of_time. A core0 that enters one while core1 holds the resource
# never comes back -- no watchdog feed, no pyro update, no fire.
# ---------------------------------------------------------------------------
UNBOUNDED = {
    "mutex_enter_blocking": "shared mutex, no timeout (mutex.c: do/while(true))",
    "recursive_mutex_enter_blocking": "shared recursive mutex, no timeout",
    "multicore_lockout_start_blocking": "waits on core1 to service its FIFO IRQ, at_the_end_of_time",
    "multicore_lockout_end_blocking": "waits on core1 to service its FIFO IRQ, at_the_end_of_time",
    "multicore_fifo_pop_blocking": "waits for core1 to push",
    "multicore_fifo_push_blocking": "waits for core1 to drain",
    "sem_acquire_blocking": "waits for a permit core1 may never return",
    "queue_add_blocking": "waits for space core1 may never make",
    "queue_remove_blocking": "waits for an entry core1 may never add",
    "critical_section_enter_blocking": "shared spin lock, no timeout",
}

# Entry points that must keep running for the rocket to be safe. These are the
# obligations: watchdog feed, launch detect, pyro service, state machine.
FLIGHT_ROOTS = [
    "main",
    "flight_update",
    "flight_update_outputs",
    "hal_pyro_update",
    "pyro_update",
    "hal_pyro_fire",
    "hal_pyro_sample",
    "action_launch",
    "hal_log_start",
    "hal_watchdog_feed",
    "net_service",
]

# Flash operations that disable XIP. While these run, the *other* core must not
# be fetching instructions from flash. Guarding them is not a call-graph
# property, so they are reported rather than failed.
XIP_DISABLERS = ["flash_range_erase", "flash_range_program"]

FN_RE = re.compile(r"^[0-9a-f]+ <(.+)>:$")
BL_RE = re.compile(r"\sbl(?:x)?\s+[0-9a-f]+ <([^>+]+)")


def find_objdump():
    for cand in ("arm-none-eabi-objdump",):
        p = shutil.which(cand)
        if p:
            return p
    import glob

    hits = sorted(glob.glob(os.path.expanduser("~/.pico-sdk/toolchain/*/bin/arm-none-eabi-objdump")))
    if hits:
        return hits[-1]
    sys.exit("arm-none-eabi-objdump not found")


def build_graph(elf):
    """callee -> set(callers), with veneers collapsed onto their target.

    The linker inserts __foo_veneer thunks for far calls. They are real nodes in
    the disassembly but not in the program's logic, so an analysis that does not
    collapse them silently reports 'no path' -- which is how a proof like this
    quietly turns into a rubber stamp."""
    out = subprocess.run([find_objdump(), "-d", elf], capture_output=True, text=True, check=True).stdout
    callers = collections.defaultdict(set)
    fn = None
    for line in out.splitlines():
        m = FN_RE.match(line)
        if m:
            fn = m.group(1)
            continue
        m = BL_RE.search(line)
        if m and fn:
            callee = m.group(1)
            if callee.startswith("__") and callee.endswith("_veneer"):
                callee = callee[2:-len("_veneer")]
            if fn.startswith("__") and fn.endswith("_veneer"):
                continue  # the veneer's own body is a jump, not a call site
            callers[callee].add(fn)
    return callers


def shortest_path(callers, target, root):
    """Reverse BFS from target; returns root..target or None."""
    prev = {target: None}
    q = collections.deque([target])
    while q:
        cur = q.popleft()
        if cur == root:
            break
        for p in callers.get(cur, ()):
            if p not in prev:
                prev[p] = cur
                q.append(p)
    if root not in prev:
        return None
    path, cur = [], root
    while cur is not None:
        path.append(cur)
        cur = prev[cur]
    return path


def analyse(elf, roots):
    callers = build_graph(elf)
    findings = []
    for prim, why in sorted(UNBOUNDED.items()):
        if prim not in callers:
            continue
        for root in roots:
            path = shortest_path(callers, prim, root)
            if path:
                findings.append((root, prim, why, path))
    xip = []
    for op in XIP_DISABLERS:
        for root in roots:
            path = shortest_path(callers, op, root)
            if path:
                xip.append((root, op, path))
                break
    return findings, xip, callers


def report(elf, roots):
    findings, xip, callers = analyse(elf, roots)
    print(f"=== {elf} ===")
    present = [p for p in UNBOUNDED if p in callers]
    print(f"unbounded primitives linked in : {', '.join(sorted(present)) or '(none)'}")
    if not findings:
        print("PASS  no flight-critical root reaches an unbounded wait")
    else:
        seen = set()
        for root, prim, why, path in findings:
            if (root, prim) in seen:
                continue
            seen.add((root, prim))
            print(f"\nFAIL  {root} can wait forever on {prim}")
            print(f"      {why}")
            print("      " + "\n        -> ".join(path))
    if xip:
        print("\nnote: these disable XIP; core1 must not be fetching from flash meanwhile")
        for root, op, path in xip:
            print(f"      {root} -> ... -> {op}  ({len(path)} frames)")
    print()
    return 0 if not findings else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="+")
    ap.add_argument("--root", action="append", default=[], help="extra flight-critical entry point")
    args = ap.parse_args()
    roots = FLIGHT_ROOTS + args.root
    rc = 0
    for e in args.elf:
        rc |= report(e, roots)
    return rc


if __name__ == "__main__":
    sys.exit(main())
