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

LIMITS. A pass means "no flight-critical root CALLS an unbounded wait". It does
not mean "core0 cannot be blocked". Hardware spin lock acquires are
__force_inline and emit no symbol, so they are reported separately and by
address, never failed. Indirect calls through function pointers are invisible
to this analysis entirely. Treat a pass as one obligation discharged, not as a
safety case.
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

# Hardware spin locks live at SIO_BASE+0x100..0x17c (32 locks, one word each).
# spin_lock_unsafe_blocking() is __force_inline and compiles to
#
#     while (__builtin_expect(!*lock, 0)) { tight_loop_contents(); }
#
# so it emits NO call and NO symbol -- a call-graph analysis cannot see it at
# all. The only trace left in the binary is the lock's address in a literal
# pool, which is what this matches. Without it, a clean call-graph result is
# not evidence of anything.
SPINLOCK_LIT_RE = re.compile(r"\bd00001[0-7][0-9a-f]\b")

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
    spin_sites = set()
    fn = None
    for line in out.splitlines():
        m = FN_RE.match(line)
        if m:
            fn = m.group(1)
            continue
        if fn and SPINLOCK_LIT_RE.search(line):
            spin_sites.add(fn)
        m = BL_RE.search(line)
        if m and fn:
            callee = m.group(1)
            if callee.startswith("__") and callee.endswith("_veneer"):
                callee = callee[2:-len("_veneer")]
            if fn.startswith("__") and fn.endswith("_veneer"):
                continue  # the veneer's own body is a jump, not a call site
            callers[callee].add(fn)
    spin_sites.discard("spin_locks_reset")  # the recovery, not a hazard
    return callers, spin_sites


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
    callers, spin_sites = build_graph(elf)
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
    spins = []
    for site in sorted(spin_sites):
        for root in roots:
            path = shortest_path(callers, site, root)
            if path:
                spins.append((root, site, path))
                break
    return findings, xip, spins, callers


# Anything core1 must never call. A core that acquires nothing can be killed
# at any instant with PSM frce_off and strand nothing, which is what makes the
# unilateral kill in src/lua/lua_core1.c safe. Spin lock acquires are inlined
# and invisible to a call graph, so the functions that contain them are named
# here directly.
CORE1_FORBIDDEN = {
    "__wrap_malloc": "system heap: enters malloc_mutex, which core0 also takes",
    "__wrap_calloc": "system heap",
    "__wrap_realloc": "system heap",
    "__wrap_free": "system heap",
    "mutex_enter_blocking": "shared mutex",
    "hw_claim_lock": "spin lock 11; a kill here strands it and hangs core0's next claim",
    "hw_claim_unused_from_range": "spin lock 11",
    "hw_claim_or_assert": "spin lock 11",
    "hw_claim_clear": "spin lock 11",
    "irq_set_exclusive_handler": "spin lock 9",
    "irq_add_shared_handler": "spin lock 9",
    "flash_range_erase": "core1 must never touch flash",
    "flash_range_program": "core1 must never touch flash",
    "multicore_fifo_pop_blocking": "unbounded wait on core0",
    "multicore_fifo_push_blocking": "unbounded wait on core0",
}


def check_core1(elf, entry, callers):
    """core1 receives resources; it never acquires them."""
    bad = []
    for prim, why in sorted(CORE1_FORBIDDEN.items()):
        path = shortest_path(callers, prim, entry)
        if path:
            bad.append((prim, why, path))
    print(f"core1 entry point       : {entry}")
    if entry not in callers and not any(entry in v for v in callers.values()):
        print(f"WARN  {entry} not found in this binary; core1 rule NOT checked")
        return 1
    if not bad:
        print(f"PASS  {entry} acquires nothing: killable at any instant")
        return 0
    for prim, why, path in bad:
        print(f"\nFAIL  core1 can call {prim}")
        print(f"      {why}")
        print("      " + "\n        -> ".join(path))
    return 1


def report(elf, roots, core1_entry=None):
    findings, xip, spins, callers = analyse(elf, roots)
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
    if spins:
        print("\nnote: these take a hardware spin lock, which this tool cannot check by")
        print("      call graph -- the acquire is inlined and emits no symbol. The SDK's")
        print("      safety argument is that such sections are short and never block, so")
        print("      the other core always finishes. Killing core1 mid-section breaks that")
        print("      argument, and spin_locks_reset() must follow any unilateral kill.")
        for root, site, path in spins:
            print(f"      {root} -> ... -> {site}  ({len(path)} frames)")
    if xip:
        print("\nnote: these disable XIP; core1 must not be fetching from flash meanwhile")
        for root, op, path in xip:
            print(f"      {root} -> ... -> {op}  ({len(path)} frames)")
    rc = 0 if not findings else 1
    if core1_entry:
        print()
        rc |= check_core1(elf, core1_entry, callers)
    print()
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="+")
    ap.add_argument("--root", action="append", default=[], help="extra flight-critical entry point")
    ap.add_argument("--core1", metavar="SYM", help="core1 entry point; fails if it acquires anything")
    args = ap.parse_args()
    roots = FLIGHT_ROOTS + args.root
    rc = 0
    for e in args.elf:
        rc |= report(e, roots, args.core1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
