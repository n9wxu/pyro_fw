#!/usr/bin/env python3
"""
Record attached boards in boards/BOARD_REGISTRY.json.

The registry is a RECORD, not an allocator. Board identity is derived from the
flash chip's unique id (see src/board_identity.h), so nothing here hands out
addresses and nothing breaks if the file is deleted -- it is rebuilt by
running this again with the hardware attached.

What it is for:
  - knowing which boards exist, and what firmware each was last given
  - catching the one case derivation cannot prevent: two boards whose derived
    subnet octet collides. That is a ~4% chance at 5 boards, 16% at 10, and it
    only bites when both are plugged in at once. Recorded here, it is visible
    before it is confusing.
  - noticing a flash swap. The identity belongs to the flash chip, not the
    PCB, so a reflowed board arrives as a new hw_id and an old one goes quiet.

Discovery uses the host's own interface addresses. Each board is a
point-to-point USB link running a DHCP server that hands the host
192.168.<octet>.2, so every such address on this machine is a board sitting at
192.168.<octet>.1. No scanning, no mDNS, no guessing.

    support/register_board.py              # discover, record, report
    support/register_board.py --dry-run    # report without writing
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REGISTRY = os.path.join(REPO, "boards", "BOARD_REGISTRY.json")


def discover():
    """Board addresses, from this host's own interfaces."""
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    # 192.168.<n>.2 on a local interface means a board at 192.168.<n>.1
    return sorted({f"192.168.{m}.1" for m in re.findall(r"inet 192\.168\.(\d+)\.2\b", out)})


def query(host, timeout=4.0):
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        return None


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"boards": []}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--registry", default=DEFAULT_REGISTRY)
    ap.add_argument("--host", action="append", help="probe this address instead of discovering")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    hosts = args.host or discover()
    if not hosts:
        print("No boards found. Each board should give this host a 192.168.<n>.2 address;")
        print("if none is present the board is not enumerated, or predates per-board subnets.")
        return 1

    reg = load(args.registry)
    by_hw = {b.get("hw_id"): b for b in reg["boards"] if b.get("hw_id")}
    today = datetime.date.today().isoformat()
    seen, changed = [], 0

    for host in hosts:
        st = query(host)
        if not st:
            print(f"  {host:<16} no response")
            continue
        hw = st.get("hw_id")
        if not hw:
            print(f"  {host:<16} {st.get('board','?')} -- firmware predates hw_id; reflash to register")
            continue

        rec = by_hw.get(hw, {})
        was = dict(rec)
        rec.update({
            "hw_id": hw,
            "board": st.get("board"),
            "mac": st.get("serial"),
            "subnet": st.get("subnet"),
            "mac_source": "override" if st.get("serial_assigned") else "derived",
            "fw_version": st.get("fw_version"),
            "last_seen": today,
        })
        rec.setdefault("first_seen", today)
        if hw not in by_hw:
            reg["boards"].append(rec)
            by_hw[hw] = rec
        if rec != was:
            changed += 1
        seen.append(rec)
        flag = "" if rec["mac_source"] == "derived" else "  [MAC overridden]"
        print(f"  {host:<16} {rec['board']:<10} hw={hw}  mac={rec['mac']}  fw={rec['fw_version']}{flag}")

    # The one thing derivation cannot rule out.
    octets = {}
    for b in reg["boards"]:
        octets.setdefault(b.get("subnet"), []).append(b)
    clashes = {k: v for k, v in octets.items() if k is not None and len(v) > 1}
    if clashes:
        print("\nSUBNET COLLISIONS -- these boards cannot be used on one host together:")
        for octet, boards in sorted(clashes.items()):
            print(f"  192.168.{octet}.x  " + ", ".join(f"{b['board']}/{b['hw_id'][:8]}" for b in boards))
        print("  Fix: POST 12 hex digits to /api/serial on one of them, then reboot.")

    if args.dry_run:
        print(f"\n(dry run; {changed} record(s) would change)")
        return 0

    reg["boards"].sort(key=lambda b: (b.get("board") or "", b.get("hw_id") or ""))
    os.makedirs(os.path.dirname(args.registry), exist_ok=True)
    with open(args.registry, "w") as f:
        json.dump(reg, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"\n{len(seen)} board(s) seen, {changed} record(s) updated -> {os.path.relpath(args.registry, REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
