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
    support/prove_flight_ram.py --emit FILE --objects DIR image.elf
        write the sections that put the closure in RAM, from an image linked
        without them, its objects under DIR: support/flight_ram_link.py does
        this at every link

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

# The flight task's loop, the states it passes through from the first
# period airborne to landing and the actions between them, the HAL tasks and
# pyro it ticks, the flight log's producer, and the mailbox call that runs
# in flight: /api/status's capture [WEB-API-08, WEB-API-11].
FLIGHT_TASK = [
    "flight_task", "dispatch_state", "flight_update_outputs", "flight_call_service", "watchdog_update",
    "flight_action_armed", "flight_action_apogee", "flight_action_landing",
    "flight_detect_ascent", "flight_detect_falling", "flight_detect_drogue_descent", "flight_detect_chute_descent",
    "hal_tasks_tick", "hal_pyro_update", "pyro_update", "hal_pyro_fire", "hal_pyro_sample",
    "hal_log_sample", "status_take_flight",
]

# Every interrupt core0 takes in flight, as the RAM vector table holds them,
# and the callbacks the timer's handler dispatches to.
INTERRUPTS = [
    "xPortPendSVHandler", "vPortSVCHandler", "prvFIFOInterruptHandler", "hardware_alarm_irq_handler",
    "period_isr", "collector_alarm_isr", "pulse_end_isr", "telem_uart_irq_handler",
    # The SDK's default alarm pool, set up on core0 before main().
    "alarm_pool_irq_handler",
]

# Tasks that can run on core0: the idle tasks have no affinity, and the
# lockout helper is pinned there.
KERNEL_TASKS = ["prvIdleTask", "prvPassiveIdleTask", "helper_task"]

ROOTS = FLIGHT_TASK + INTERRUPTS + KERNEL_TASKS

# Roots one board's image has and another's does not.
OPTIONAL_ROOTS = {"pulse_end_isr", "collector_alarm_isr", "telem_uart_irq_handler", "alarm_pool_irq_handler"}

# Pointers stored at run time, where no table in the image holds them: the
# caller's indirect branch reaches these. flight_call_service() runs only a
# function in RAM in flight (rtos_tasks.c, may_run()).
#
# The buzzer's task is not among hal_tasks_tick()'s: its tick runs only while
# a sound plays, entering flight stops the buzzer (main_hardware.c), and
# nothing starts one again until landing has ended flight.
PYRO_OPS = ["real_fire", "real_get", "real_fault", "mock_fire", "mock_get", "mock_fault"]
RUNTIME_TARGETS = {
    "hal_tasks_tick": ["pres_tick", "gts_tick"],
    "hardware_alarm_irq_handler": ["period_isr", "collector_alarm_isr", "pulse_end_isr"],
    # The one callback the image puts in that pool (pico_time, time.c).
    "alarm_pool_irq_handler": ["sleep_until_callback"],
    "flight_call_service": [],
    "hal_pyro_fire": PYRO_OPS,
    "hal_pyro_get": PYRO_OPS,
    "hal_pyro_fault": PYRO_OPS,
    "mock_fire": ["report_mock"],
    "mock_get": ["report_mock"],
    # Through estimators_vt, whose entries the table scan follows.
    "pp_feed_us": [],
    "pp_note_pulse": [],
    "pp_noise_pa": [],
    # Through detectors[] and transitions[], whose in-flight entries are roots.
    "dispatch_state": [],
    # The return into the task the switch chose.
    "xPortPendSVHandler": [],
}

# The SDK's float, double and memory shims call the ROM's routines through
# its function table: no flash.
ROM_CALLERS_PREFIX = "__wrap_"

# Tables whose entries for the flight's states are roots above: the rest are
# the pad's and the boot's, and are not followed.
ROOTED_TABLES = {"detectors", "transitions"}

# Calls a root makes that cannot happen between the first period airborne
# and landing, each with its reason.
CUTS = {
    ("flight_task", "flight_task_start"): "once, before the loop",
    ("flight_task", "service_reset"): "a reset asked in flight waits for landing",
    ("flight_set_usb_attached", "flight_grounded_on_usb"): "it returns at once while airborne",
    ("flight_set_usb_attached", "buzzer_play_usb_ok"): "it returns at once while airborne",
    ("flight_set_usb_attached", "buzzer_play_altitude"): "it returns at once while airborne",
    ("buzzer_play_altitude", "buzzer_tick"): "stored for the buzzer's task, which ticks it after landing",
    ("buzzer_play_spec", "buzzer_tick"): "stored for the buzzer's task: a fault ends flight first",
    ("follow_the_pad", "air_over_pad"): "the pad's pressure and temperature are frozen at launch: it returns first",
}

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

    def section_at(self, addr, text=False):
        """The input section holding addr. Merged string sections share
        addresses in the map; text=True prefers a function's own."""
        i = bisect.bisect_right(self.sec_starts, addr) - 1
        hits = []
        while i >= 0 and self.sections[i][0] + 0x10000 > addr:
            s = self.sections[i]
            if s[0] <= addr < s[0] + s[1]:
                hits.append(s)
            i -= 1
        if text:
            code = [s for s in hits if ".text" in s[2]]
            hits = code or hits
        # The map gives a merged string section the place its first string
        # took, whatever else lies there: anything else is the better answer.
        whole = [s for s in hits if ".str1." not in s[2]]
        hits = whole or hits
        return hits[0] if hits else None

    def string_at(self, addr):
        """The NUL-terminated bytes at addr, as linked."""
        out = bytearray()
        while len(out) < 512:
            b = self.mem.get(addr + len(out))
            if b is None or b == 0:
                break
            out.append(b)
        return bytes(out)

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
        self.anon_obj = set()  # the objects whose tables read those addresses
        self.indirect = []  # (function, line) not covered by a rule
        self.via_table = collections.defaultdict(set)  # target -> table it came through
        self.parent = {}  # function -> the function or table that first reached it

    def walk(self, roots):
        img = self.img
        todo = [a for r in roots for a in img.by_name.get(r, [])]
        while todo:
            a = todo.pop()
            if a in self.code:
                continue
            self.code.add(a)
            name, lines = img.fns[a]
            self.current = name
            if name in LEAVES or a < ROM_HI:
                continue
            covered = name in RUNTIME_TARGETS
            for t in RUNTIME_TARGETS.get(name, []):
                todo.extend(img.by_name.get(t, []))
            recent = []
            for line in lines:
                recent = (recent + [line])[-3:]
                m = pc.BRANCH_RE.search(line)
                if m:
                    target, tname = int(m.group(2), 16), m.group(3).split("+")[0]
                    if tname.startswith("__") and tname.endswith("_veneer"):
                        landed = img.by_name.get(tname[2:-len("_veneer")])
                        target = landed[0] if landed else target
                    elif "+" in m.group(3):
                        continue
                    if target in img.fns and target != a and (name, img.fns[target][0]) not in CUTS:
                        self.parent.setdefault(target, name)
                        todo.append(target)
                    continue
                if pc.INDIRECT_RE.search(line) and not re.search(r"\sbx\s+lr\b", line):
                    switch = re.search(r"\smov\s+pc,", line)  # GCC's switch, through a table in the function
                    reg = re.search(r"\sbx\s+(r\d+)", line)
                    ret = reg and any(re.search(r"\spop\s+\{[^}]*\b" + reg.group(1) + r"\b", r) for r in recent[:-1])
                    veneer = name.endswith("_veneer")  # the linker's jump to the function it is named for
                    if not (covered or switch or ret or veneer or name.startswith(ROM_CALLERS_PREFIX)):
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
                if img.fns[f][0] != reader:
                    self.parent.setdefault(f, reader + " (address)")
                todo.append(f)
            return
        if not (in_flash(addr) or SRAM_LO <= addr < SRAM_HI):
            return
        o = img.object_at(addr)
        if o is None:
            sec = img.section_at(addr)
            placed = sec is not None and sec[2].startswith(PLACED + ".rodata")
            if (in_flash(addr) or placed) and img.function_at(addr) is None:
                self.anon.setdefault(addr, reader)
                if depth:
                    self.anon_obj.add(reader)
            return
        if o in self.objects or depth > 4:
            return
        self.objects.add(o)
        start, size, oname = o
        if oname in ROOTED_TABLES:
            return
        for off in range(0, size - 3, 4):
            w = img.word(start + off)
            tf = img.function_at(w)
            if tf is not None and (w & 1):
                self.via_table[tf].add(oname)
                self.parent.setdefault(tf, f"{reader} via {oname}")
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
    anon = {a: r for a, r in cl.anon.items() if in_flash(a)}
    missing = [r for r in roots if r not in img.by_name and r not in OPTIONAL_ROOTS]
    print(f"=== {elf} ===")
    print(f"closure: {len(code)} functions, {total} bytes of code; {len(flash)} in flash, {flash_bytes} bytes")
    print(f"read-only data in flash the path reads: {len(ro)} objects, {ro_bytes} bytes; "
          f"{len(anon)} other flash addresses (strings, literals)")
    print(f"indirect branches no rule covers: {len(cl.indirect)} in {len(set(f for f, _ in cl.indirect))} functions")
    if missing:
        print(f"roots not in the image: {', '.join(missing)}")
    if args.list:
        for a in sorted(code, key=lambda x: -img.sizes.get(x, 0)):
            where = "RAM  " if SRAM_LO <= a < SRAM_HI else "flash"
            sec = img.section_at(a, text=True)
            obj = os.path.basename(sec[3]) if sec else "?"
            print(f"  {img.sizes.get(a, 0):6d} {where} {img.fns[a][0]}  [{obj}]")
        for o in sorted(ro, key=lambda o: -o[1]):
            print(f"  {o[1]:6d} rodata {o[2]}")
        for addr, reader in sorted(anon.items()):
            sec = img.section_at(addr)
            print(f"  anon 0x{addr:08x} read by {reader}: {sec[2] if sec else '?'} {os.path.basename(sec[3]) if sec else ''}")
        for f, line in cl.indirect:
            print(f"  indirect: {f}: {line}")
    return 1 if (args.strict and (flash or ro or anon or cl.indirect or missing)) else 0


PLACED = ".time_critical.flight"  # support/flight_ram_link.py's prefix
PLACED_ANY = (PLACED,)


def unplaced(name):
    return name[len(PLACED):] if name.startswith(PLACED) else name


def section_entry(sec, name=None):
    """The list's line for this section: its object, and its name as the
    compiler wrote it."""
    obj = sec[3]
    name = unplaced(name or sec[2])
    m = re.match(r"(.*\.a)\((.*)\)$", obj) or re.match(r".*/lib/([^/]+\.a)/([^/]+)$", obj)
    key = f"{os.path.basename(m.group(1))}:{m.group(2)}" if m else os.path.basename(obj)
    return f"{key} {name}"


def ours(img, a):
    """Placed in RAM by the list, or still in flash: either way the list's."""
    if not (SRAM_LO <= a < SRAM_HI):
        return True
    sec = img.section_at(a, text=True)
    return bool(sec and sec[2].startswith(PLACED_ANY))


class Objects:
    """The build's object files, found by the names the link map gives them,
    and the sections each section of theirs refers to."""

    def __init__(self, build_dir, objdump):
        self.objdump, self.paths, self.cache = objdump, collections.defaultdict(list), {}
        for root, _dirs, files in os.walk(build_dir):
            for f in files:
                if f.endswith((".o", ".obj", ".a")):
                    self.paths[f].append(os.path.join(root, f))

    def relocs(self, key):
        """{section: {what it refers to}} for the object key names, merged
        over the objects a name could mean."""
        if key in self.cache:
            return self.cache[key]
        arc, _, member = key.partition(":")
        out = collections.defaultdict(set)
        for path in self.paths.get(arc, []):
            dump = subprocess.run([self.objdump, "-r", path], capture_output=True, text=True).stdout
            cur, mine = None, not member
            for line in dump.splitlines():
                if member and line.endswith("file format elf32-littlearm"):
                    mine = line.split(":")[0].strip() == member
                    cur = None
                    continue
                m = re.match(r"RELOCATION RECORDS FOR \[(\S+)\]:", line)
                if m:
                    cur = m.group(1)
                    continue
                parts = line.split()
                if mine and cur and len(parts) == 3 and re.fullmatch(r"[0-9a-f]+", parts[0]):
                    out[cur].add(parts[2].split("+")[0].split("-")[0])
        self.cache[key] = out
        return out

    def rodata(self, key, section):
        """Every read-only section that section refers to, and theirs in turn."""
        seen, todo, rel = set(), [section], self.relocs(key)
        while todo:
            for t in rel.get(todo.pop(), ()):
                if t.startswith(".rodata") and t not in seen:
                    seen.add(t)
                    todo.append(t)
        return seen


# The SDK's assembly shims branch between their own sections with short
# branches, so a file goes whole: the SDK's PICO_*_IN_RAM switches put them
# in RAM (CMakeLists.txt), and the list leaves them out.
WHOLE_FILES = re.compile(r"\.S\.o$")

# A C library built without -ffunction-sections (Ubuntu's newlib) keeps each
# function in its member's .text.
C_LIBRARY = re.compile(r"^lib[cgm](_nano)?\.a:")


def whole_member(img, cl, sec):
    """A C library member's .text, holding no function off the path."""
    if unplaced(sec[2]) != ".text" or not C_LIBRARY.match(section_entry(sec)):
        return False
    lo = bisect.bisect_left(img.fn_starts, sec[0])
    hi = bisect.bisect_left(img.fn_starts, sec[0] + sec[1])
    return all(f in cl.code for f in img.fn_starts[lo:hi])


def placement(img, cl, objs):
    """The input sections that put the closure, and what it reads, in RAM."""
    pats = []
    for a in sorted(cl.code):
        if a < ROM_HI or not ours(img, a) or img.fns[a][0].endswith("_veneer"):
            continue  # the linker makes veneers where it needs them
        sec = img.section_at(a, text=True)
        if sec and WHOLE_FILES.search(sec[3]):
            continue
        name = unplaced(sec[2]) if sec else ""
        if not (name.startswith(".text.") or (sec and whole_member(img, cl, sec))):
            sys.exit(f"no input section of its own for {img.fns[a][0]} at 0x{a:08x}")
        entry = section_entry(sec)
        pats.append(entry)
        key = entry.split(" ")[0]
        pats += [f"{key} {r}" for r in objs.rodata(key, name)]
    # A table another object holds, and what its entries point at. String
    # literals go by relocation: the map gives a merged string no owner.
    for o in sorted(cl.objects):
        sec = img.section_at(o[0])
        name = unplaced(sec[2]) if sec else ""
        if name.startswith(".rodata") or name.startswith(".data"):
            entry = section_entry(sec)
            key = entry.split(" ")[0]
            if name.startswith(".rodata"):
                pats.append(entry)
            pats += [f"{key} {r}" for r in objs.rodata(key, name)]
    return pats


def emit(path, pats):
    """The list support/flight_ram_link.py renames into RAM at the link."""
    pats = sorted(set(pats))
    with open(path, "w") as f:
        f.write("# The flight path in RAM [FLT-RT-02, DD-104], written by\n"
                "# support/prove_flight_ram.py --emit. An object and one of its sections a line.\n")
        for p in pats:
            f.write(p + "\n")
    print(f"wrote {len(pats)} input sections to {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="+")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    ap.add_argument("--list", action="store_true", help="every function in the closure, by size")
    ap.add_argument("--emit", metavar="FILE", help="write the closure's placement for ld")
    ap.add_argument("--root", action="append", default=[], help="an extra root")
    ap.add_argument("--why", action="append", default=[], metavar="FN", help="how the closure reaches FN")
    ap.add_argument("--objdump", help="the toolchain's objdump")
    ap.add_argument("--objects", metavar="DIR", help="where the image's object files are (default: beside it)")
    args = ap.parse_args()
    roots = ROOTS + args.root
    rc, pats = 0, []
    objdump = args.objdump or pc.find_objdump()
    for elf in args.elf:
        img = Image(elf, objdump)
        objs = Objects(args.objects or os.path.dirname(os.path.abspath(elf)), objdump)
        cl = Closure(img)
        cl.walk([r for r in roots if r in img.by_name])
        rc |= report(img, cl, roots, args, elf)
        names = {img.fns[a][0]: a for a in cl.code}
        for fn in args.why:
            chain, a = [fn], names.get(fn)
            while a is not None and a in cl.parent:
                p = cl.parent[a]
                chain.append(p)
                a = names.get(p.split(" ")[0])
                if len(chain) > 40:
                    break
            print("why " + " <- ".join(chain))
        if args.emit:
            pats += placement(img, cl, objs)
    if args.emit:
        emit(args.emit, pats)
    return rc


if __name__ == "__main__":
    sys.exit(main())
