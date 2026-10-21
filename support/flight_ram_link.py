#!/usr/bin/env python3
"""
flight_ram_link.py -- link the flight path into RAM [FLT-RT-02, DD-104].

CMake runs every firmware link through this (RULE_LAUNCH_LINK):

    flight_ram_link.py OBJCOPY -- <the link command>

It links twice. The first link is the command as given, to a scratch image;
support/prove_flight_ram.py finds that image's flight path and writes the
input sections that hold it, "<object> <section>" a line, the object an
object file's name or "<archive>:<member>" (<image>.flight_ram.txt, kept
beside the image). The second link is the command again, with each object
that has some of those sections replaced by a copy in which they are
renamed .time_critical.flight.*, which the linker script already places in
.data: copied to RAM at start-up. An archive's copy has the sections renamed
in every member. A member of the C library, which the compiler driver adds
itself, is taken out of the library, renamed, and linked ahead of it.

The list follows the code and the compiler, at every link. CI's prover then
checks the image (prove_flight_ram.py --strict).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PREFIX = ".time_critical.flight"
DRIVER_LIBS = ("libc.a", "libg.a", "libc_nano.a", "libg_nano.a", "libm.a")


def read_list(path):
    """{object: [section]}"""
    by_obj = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                obj, sec = line.split()
                by_obj.setdefault(obj, []).append(sec)
    return by_obj


def tool_beside(objcopy, name):
    return re.sub(r"objcopy(\.exe)?$", name + r"\1", objcopy)


def objcopy(tool, src, dst, sections):
    args = [tool]
    for s in sections:
        args += ["--rename-section", f"{s}={PREFIX}{s}"]
    subprocess.run(args + [src, dst], check=True)


def driver_lib(cmd, name):
    """Where the compiler driver finds the C library for this link's flags."""
    flags = [a for a in cmd[1:] if a.startswith(("-mcpu", "-mthumb", "-march", "-mfloat-abi", "-mfpu", "--specs"))]
    out = subprocess.run([cmd[0]] + flags + [f"-print-file-name={name}"], capture_output=True, text=True)
    path = out.stdout.strip()
    return path if os.path.isabs(path) and os.path.exists(path) else None


def retarget(cmd, elf):
    """The command, writing elf and its map in place of the image's."""
    out = list(cmd)
    out[out.index("-o") + 1] = elf
    return [re.sub(r"^-Wl,-Map=.*$", f"-Wl,-Map={elf}.map", a) for a in out]


def placed(cmd, by_obj, tool, tmp):
    """The command, each object with listed sections replaced by its copy."""
    archives = {}
    for key, secs in by_obj.items():
        if ":" in key:
            archives.setdefault(key.split(":", 1)[0], []).extend(secs)
    out = []

    def copy_of(path, secs):
        """Under its own name, which the link map then reports."""
        d = os.path.join(tmp, str(len(out)))
        os.makedirs(d)
        dst = os.path.join(d, os.path.basename(path))
        objcopy(tool, path, dst, secs)
        return dst

    for a in cmd:
        base = os.path.basename(a)
        if os.path.isfile(a) and base in by_obj:
            out.append(copy_of(a, by_obj[base]))
        elif os.path.isfile(a) and base in archives:
            out.append(copy_of(a, sorted(set(archives[base]))))
        else:
            out.append(a)
    extra = []
    for key, secs in by_obj.items():
        arc, _, member = key.partition(":")
        if arc not in DRIVER_LIBS or not member:
            continue
        lib = driver_lib(cmd, arc)
        if not lib:
            sys.exit(f"flight_ram_link: {arc} not found for {member}")
        src = os.path.join(tmp, f"x_{member}")
        with open(src, "wb") as f:
            f.write(subprocess.run([tool_beside(tool, "ar"), "p", lib, member], capture_output=True,
                                   check=True).stdout)
        os.makedirs(os.path.join(tmp, "lib", arc), exist_ok=True)
        dst = os.path.join(tmp, "lib", arc, member)  # the map reports it as <arc>/<member>
        objcopy(tool, src, dst, secs)
        extra.append(dst)
    at = out.index("-o")
    return out[:at] + extra + out[at:]


def main():
    argv = sys.argv[1:]
    sep = argv.index("--")
    tool, cmd = argv[0], argv[sep + 1:]
    image = cmd[cmd.index("-o") + 1]
    tmp = tempfile.mkdtemp(prefix="flight_ram_")
    try:
        first = os.path.join(tmp, "first.elf")
        rc = subprocess.run(retarget(cmd, first)).returncode
        if rc:
            return rc
        listing = os.path.abspath(image) + ".flight_ram.txt"
        subprocess.run([sys.executable, os.path.join(HERE, "prove_flight_ram.py"), "--emit", listing,
                        "--objdump", tool_beside(tool, "objdump"), "--objects", os.getcwd(), first],
                       check=True, stdout=subprocess.DEVNULL)
        return subprocess.run(placed(cmd, read_list(listing), tool, tmp)).returncode
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
