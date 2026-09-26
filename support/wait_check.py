#!/usr/bin/env python3
"""Fail if the firmware sleeps [DD-053].

    support/wait_check.py

The exec loop is the only clock: anything that has to wait parks on a
deadline and a later iteration picks it up. The only interruptions are flash
writes. So no source in src/ or boards/ may call a function whose job is to
let time pass: the SDK's sleeps and busy-waits, and the blocking DMA and PIO
waits that pace a transfer by time.

ALLOWED lists calls not yet converted, per file, as a ratchet: a file over
its count fails, and so does a file under it, so the list is kept exact and
only shrinks. It is empty: every sleep in the tree is gone.

Outside this check: bounded waits on a bus or a peripheral's handshake, such
as the I2C drivers' transfers and core1's power-state acknowledgement. They
wait for hardware, not for time.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["src", "boards"]
SKIP = {"lua-5.4", "third_party"}

WAITS = [
    "sleep_ms", "sleep_us", "sleep_until",
    "busy_wait_us", "busy_wait_us_32", "busy_wait_ms", "busy_wait_until",
    "best_effort_wfe_or_timeout",
    "dma_channel_wait_for_finish_blocking",
    "pio_sm_put_blocking", "pio_sm_get_blocking",
]
CALL_RE = re.compile(r"\b(" + "|".join(WAITS) + r")\s*\(")

ALLOWED = {}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def calls():
    found = {}
    for d in DIRS:
        for dirpath, dirnames, files in os.walk(os.path.join(ROOT, d)):
            dirnames[:] = [x for x in dirnames if x not in SKIP]
            for f in files:
                if not f.endswith((".c", ".h")):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, ROOT)
                lines = strip_comments(open(path, encoding="utf-8", errors="replace").read()).split("\n")
                for n, line in enumerate(lines, 1):
                    for m in CALL_RE.finditer(line):
                        found.setdefault(rel, []).append((n, m.group(1)))
    return found


def main():
    found = calls()
    problems = 0
    for rel in sorted(set(found) | set(ALLOWED)):
        sites = found.get(rel, [])
        allowed = ALLOWED.get(rel, 0)
        if len(sites) > allowed:
            problems += 1
            print(f"{rel}: {len(sites)} waits, {allowed} allowed")
            for n, name in sites:
                print(f"    {rel}:{n}  {name}()")
        elif len(sites) < allowed:
            problems += 1
            print(f"{rel}: {len(sites)} waits, but ALLOWED says {allowed}: lower it")
    left = sum(ALLOWED.values())
    print(f"wait_check: {problems} problem(s); {left} wait(s) still to convert")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
