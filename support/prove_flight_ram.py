#!/usr/bin/env python3
"""
prove_flight_ram.py -- the flight path from launch to landing, in RAM [FLT-RT-02, DD-104].

From launch to landing the flight software -- the flight task, every
interrupt handler core0 takes, and the kernel paths they use -- fetches no
instruction and reads no data from the boot flash, so the log writer's flash
operations on core1 cannot disturb it. That is a property of the linked
image, so this works on the ELF, with prove_core0.py's disassembly.

From each root it follows every direct branch, every function pointer held
in a table the path reads, and the targets named in RUNTIME_TARGETS for
pointers set at run time. It reports each function the closure reaches in
flash, each indirect branch no rule covers, and each flash address the path
reads.

Usage:
    support/prove_flight_ram.py build-mk1c/pyro_fw_mk1c.elf            report
    support/prove_flight_ram.py --list build-mk1c/pyro_fw_mk1c.elf     and every function, by size
    support/prove_flight_ram.py --strict build-mk1c/pyro_fw_mk1c.elf   fail on any finding
    support/prove_flight_ram.py --emit FILE build-mk1c/pyro_fw_mk1c.elf
        write the linker's placement of the closure (ld --section-ordering-file)

The link map (<elf>.map) names each function's input section and object.

Functions are keyed by address: two static functions may share a name (the
SDK's panic() and Lua's), and merging them by name invents paths.

LIMITS. A function a root calls is followed whatever branch of it runs in
flight, so the closure over-counts: a function's pad branch is in it. Calls
that cannot happen between launch and landing are cut by name in CUTS, each
with its reason; a cut is a claim about the code, checked by review.
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prove_core0 as pc  # noqa: E402

# The flight task's loop and the states it passes through from launch to
# landing, the HAL tasks and pyro it ticks, and the flight log's producer.
FLIGHT_TASK = [
    "flight_task", "dispatch_state", "flight_update_outputs", "flight_call_service", "watchdog_update",
    "flight_action_launch", "flight_action_armed", "flight_action_apogee", "flight_action_landing",
    "flight_action_resumed_descent", "flight_detect_ascent", "flight_detect_falling",
    "flight_detect_drogue_descent", "flight_detect_chute_descent", "flight_detect_landed",
    "hal_tasks_tick", "hal_pyro_update", "pyro_update", "hal_pyro_fire", "hal_pyro_sample",
    "hal_log_start", "hal_log_sample",
]

# Every interrupt core0 takes in flight, as the RAM vector table holds them,
# and the callbacks the timer's handler dispatches to.
INTERRUPTS = [
    "xPortPendSVHandler", "vPortSVCHandler", "prvFIFOInterruptHandler", "hardware_alarm_irq_handler",
    "period_isr", "collector_alarm_isr", "pulse_end_isr", "telem_uart_irq_handler",
]

# Tasks that can run on core0: the idle tasks have no affinity, and the
# lockout helper is pinned there.
KERNEL_TASKS = ["prvIdleTask", "prvPassiveIdleTask", "helper_task"]

ROOTS = FLIGHT_TASK + INTERRUPTS + KERNEL_TASKS

# Roots one board's image has and another's does not.
OPTIONAL_ROOTS = {"pulse_end_isr", "collector_alarm_isr", "telem_uart_irq_handler"}

# Pointers stored at run time, where no table in the image holds them: the
# caller's indirect branch reaches these.
RUNTIME_TARGETS = {
    "hal_tasks_tick": ["pres_tick", "pres_bringup_tick", "buzzer_tick", "gts_tick"],
    "hardware_alarm_irq_handler": ["period_isr", "collector_alarm_isr", "pulse_end_isr"],
}

# Calls a root makes that cannot happen between launch and landing.
CUTS = {}

# Paths that end the program: followed no further.
LEAVES = {"panic", "__assert_func", "hard_assertion_failure", "panic_unsupported", "pyro_panic",
          "rtos_assert_failed", "vApplicationStackOverflowHook"}

ROM_HI = 0x00004000
FLASH_LO, FLASH_HI = pc.XIP_LO, 0x11000000
SRAM_LO, SRAM_HI = pc.RAM_LO, pc.RAM_HI

MAP_SECTION_RE = re.compile(r"^ (\.\S+)\s*$")
MAP_PLACED_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
MAP_ONE_LINE_RE = re.compile(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")


class Image:
    def __init__(self, elf, objdump):
        out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True, check=True).stdout
        self.fns, self.by_name, cur = {}, collections.defaultdict(list), None
        for line in out.splitlines():
            m = pc.FN_ADDR_RE.match(line)
            if m:
                cur = int(m.group(1), 16)
                self.fns[cur] = (m.group(2), [])
                self.by_name[m.group(2)].append(cur)
            elif cur is not None and line.strip():
                self.fns[cur][1].append(line)
        self.sizes, self.objects = {}, []
        syms = subprocess.run([objdump, "-t", elf], capture_output=True, text=True, check=True).stdout
        for line in syms.splitlines():
            m = pc.SYM_FUNC_RE.match(line)
            if m:
                self.sizes[int(m.group(1), 16) & ~1] = int(m.group(2), 16)
                continue
            m = pc.SYM_OBJ_RE.match(line)
            if m and int(m.group(2), 16):
                self.objects.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
        self.objects.sort()
        self.obj_starts = [o[0] for o in self.objects]
        self.fn_starts = sorted(self.fns)
        self.mem = pc.section_bytes(elf, objdump)
        self.sections = self.read_map(elf + ".map")
        self.sec_starts = [s[0] for s in self.sections]

    @staticmethod
    def read_map(path):
        """(start, size, section, object) for every input section placed."""
        out, pending = [], None
        if not os.path.exists(path):
            return out
        with open(path, errors="replace") as f:
            for line in f:
                m = MAP_ONE_LINE_RE.match(line)
                if m:
                    out.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1), m.group(4).strip()))
                    pending = None
                    continue
                m = MAP_SECTION_RE.match(line)
                if m:
                    pending = m.group(1)
                    continue
                m = MAP_PLACED_RE.match(line)
                if m and pending:
                    out.append((int(m.group(1), 16), int(m.group(2), 16), pending, m.group(3).strip()))
                pending = None
        return sorted(s for s in out if s[1])

    def function_at(self, addr):
        a = addr & ~1
        return a if a in self.fns else None

    def object_at(self, addr):
        i = bisect.bisect_right(self.obj_starts, addr) - 1
        if i >= 0:
            start, size, name = self.objects[i]
            if start <= addr < start + size:
                return self.objects[i]
        return None

    def section_at(self, addr):
        i = bisect.bisect_right(self.sec_starts, addr) - 1
        if i >= 0:
            s = self.sections[i]
            if s[0] <= addr < s[0] + s[1]:
                return s
        return None

    def word(self, addr):
        return int.from_bytes(bytes(self.mem.get(addr + i, 0) for i in range(4)), "little")


def in_flash(a):
    return FLASH_LO <= a < FLASH_HI


class Closure:
    def __init__(self, img):
        self.img = img
        self.code = set()
        self.objects = set()  # (start, size, name) read by the path
        self.anon = {}  # flash address read that no symbol covers -> reader
        self.indirect = []  # (function, line) not covered by a rule
        self.via_table = collections.defaultdict(set)  # target -> table it came through

    def walk(self, roots):
        img = self.img
        todo = [a for r in roots for a in img.by_name.get(r, [])]
        while todo:
            a = todo.pop()
            if a in self.code:
                continue
            self.code.add(a)
            name, lines = img.fns[a]
            if name in LEAVES or a < ROM_HI:
                continue
            covered = name in RUNTIME_TARGETS
            for t in RUNTIME_TARGETS.get(name, []):
                todo.extend(img.by_name.get(t, []))
            for line in lines:
                m = pc.BRANCH_RE.search(line)
                if m:
                    target, tname = int(m.group(2), 16), m.group(3).split("+")[0]
                    if tname.startswith("__") and tname.endswith("_veneer"):
                        landed = img.by_name.get(tname[2:-len("_veneer")])
                        target = landed[0] if landed else target
                    elif "+" in m.group(3):
                        continue
                    if target in img.fns and target != a and (name, img.fns[target][0]) not in CUTS:
                        todo.append(target)
                    continue
                if pc.INDIRECT_RE.search(line) and not re.search(r"\sbx\s+lr\b", line):
                    if not covered:
                        self.indirect.append((name, line.strip()))
                    continue
                m = pc.WORD_RE.search(line)
                if m:
                    self.read(int(m.group(1), 16), name, todo, depth=0)

    def read(self, addr, reader, todo, depth):
        img = self.img
        f = img.function_at(addr)
        if f is not None and (addr & 1):
            if (reader, img.fns[f][0]) not in CUTS:
                todo.append(f)
            return
        if not (in_flash(addr) or SRAM_LO <= addr < SRAM_HI):
            return
        o = img.object_at(addr)
        if o is None:
            if in_flash(addr) and img.function_at(addr) is None:
                self.anon.setdefault(addr, reader)
            return
        if o in self.objects or depth > 4:
            return
        self.objects.add(o)
        start, size, oname = o
        for off in range(0, size - 3, 4):
            w = img.word(start + off)
            tf = img.function_at(w)
            if tf is not None and (w & 1):
                self.via_table[tf].add(oname)
                todo.append(tf)
            elif in_flash(w) or SRAM_LO <= w < SRAM_HI:
                self.read(w, oname, todo, depth + 1)


def report(img, cl, roots, args, elf):
    code = [a for a in cl.code if a >= ROM_HI]
    flash = [a for a in code if not (SRAM_LO <= a < SRAM_HI)]
    total = sum(img.sizes.get(a, 0) for a in code)
    flash_bytes = sum(img.sizes.get(a, 0) for a in flash)
    ro = [o for o in cl.objects if in_flash(o[0])]
    ro_bytes = sum(o[1] for o in ro)
    missing = [r for r in roots if r not in img.by_name and r not in OPTIONAL_ROOTS]
    print(f"=== {elf} ===")
    print(f"closure: {len(code)} functions, {total} bytes of code; {len(flash)} in flash, {flash_bytes} bytes")
    print(f"read-only data in flash the path reads: {len(ro)} objects, {ro_bytes} bytes; "
          f"{len(cl.anon)} other flash addresses (strings, literals)")
    print(f"indirect branches no rule covers: {len(cl.indirect)} in {len(set(f for f, _ in cl.indirect))} functions")
    if missing:
        print(f"roots not in the image: {', '.join(missing)}")
    if args.list:
        for a in sorted(code, key=lambda x: -img.sizes.get(x, 0)):
            where = "RAM  " if SRAM_LO <= a < SRAM_HI else "flash"
            sec = img.section_at(a)
            obj = os.path.basename(sec[3]) if sec else "?"
            print(f"  {img.sizes.get(a, 0):6d} {where} {img.fns[a][0]}  [{obj}]")
        for o in sorted(ro, key=lambda o: -o[1]):
            print(f"  {o[1]:6d} rodata {o[2]}")
        for addr, reader in sorted(cl.anon.items()):
            sec = img.section_at(addr)
            print(f"  anon 0x{addr:08x} read by {reader}: {sec[2] if sec else '?'} {os.path.basename(sec[3]) if sec else ''}")
        for f, line in cl.indirect:
            print(f"  indirect: {f}: {line}")
    return 1 if (args.strict and (flash or ro or cl.anon or cl.indirect or missing)) else 0


def section_pattern(sec):
    """An input-section pattern ld matches to this one section and no other."""
    start, size, name, obj = sec
    m = re.match(r"(.*\.a)\((.*)\)$", obj)
    if m:
        return f"*{os.path.basename(m.group(1))}:{m.group(2)}({name})"
    return f"*{os.path.basename(obj)}({name})"


def emit(img, cl, path):
    """The closure's code and read-only data into .data, which the startup
    code copies to RAM, ahead of the linker script's own placement."""
    pats = []
    for a in sorted(cl.code):
        if a < ROM_HI or SRAM_LO <= a < SRAM_HI:
            continue
        sec = img.section_at(a)
        if sec is None or not sec[2].startswith(".text"):
            sys.exit(f"no input section of its own for {img.fns[a][0]} at 0x{a:08x}")
        pats.append(section_pattern(sec))
    for o in sorted(cl.objects):
        if in_flash(o[0]):
            sec = img.section_at(o[0])
            if sec and sec[2].startswith(".rodata"):
                pats.append(section_pattern(sec))
    for addr in sorted(cl.anon):
        sec = img.section_at(addr)
        if sec and sec[2].startswith(".rodata"):
            pats.append(section_pattern(sec))
    pats = list(dict.fromkeys(pats))
    with open(path, "w") as f:
        f.write("/* Generated by support/prove_flight_ram.py --emit [FLT-RT-02]. */\nSECTIONS\n{\n  .data : {\n")
        for p in pats:
            f.write(f"    {p}\n")
        f.write("  }\n}\n")
    print(f"wrote {len(pats)} input sections to {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    ap.add_argument("--list", action="store_true", help="every function in the closure, by size")
    ap.add_argument("--emit", metavar="FILE", help="write the closure's placement for ld")
    ap.add_argument("--root", action="append", default=[], help="an extra root")
    args = ap.parse_args()
    img = Image(args.elf, pc.find_objdump())
    roots = ROOTS + args.root
    cl = Closure(img)
    cl.walk([r for r in roots if r in img.by_name])
    rc = report(img, cl, roots, args, args.elf)
    if args.emit:
        emit(img, cl, args.emit)
    return rc


if __name__ == "__main__":
    sys.exit(main())
