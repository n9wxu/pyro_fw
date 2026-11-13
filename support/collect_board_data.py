#!/usr/bin/env python3
"""Collects everything a Pyro board can say about itself into one zip file,
ready to e-mail.

    python collect_board_data.py
    python collect_board_data.py --note "three beeps, pause, repeating, on a charger"

Plug the board into this computer by USB first. If the board is making a
sound or doing something you want explained, run this BEFORE removing its
power: a restart loses what the board is doing now.

The program only reads from the board. It changes nothing on it, erases
nothing and does not restart it. It needs Python 3.8 or later and nothing
else, on Windows, macOS or Linux.

What goes into the zip, per board:
  status_1.json .. status_3.json   what the board reports, three times over
                                   a few seconds, so a changing state shows
  fs.json, config, pins, beeps...  its settings and its file list
  files/                           every file it holds but the web pages:
                                   each flight log, the boot log, settings
  flight.csv                       the newest flight as the web page gives it
  pressure_trace/                  about ten seconds of raw sensor readings
  manifest.json                    what was fetched, what failed and why
"""
import argparse
import io
import json
import platform
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

HOST_SIDE = re.compile(r"(?:inet |IPv4[^:\n]*:\s*)192\.168\.(\d+)\.2\b")
STATUS_READS = 3
STATUS_GAP_S = 3
TRACE_S = 10
JSON_PAGES = {
    "fs.json": "/api/fs",
    "config.txt": "/api/config",
    "pins.txt": "/api/pins",
    "pins_caps.json": "/api/pins/caps",
    "beeps.txt": "/api/beeps",
    "limits.json": "/api/limits",
    "log_space.json": "/api/log/space",
    "net.json": "/api/net",
    "lua_console.json": "/api/lua/console",
    "sim.json": "/api/sim",
    "sd.json": "/api/sd",
    "hr.json": "/api/hr",
}


def interfaces():
    """This computer's network addresses, as its own tool prints them."""
    system = platform.system()
    if system == "Windows":
        commands = [["ipconfig"]]
    elif system == "Darwin":
        commands = [["ifconfig"]]
    else:
        commands = [["ip", "-4", "addr"], ["ifconfig"]]
    for command in commands:
        try:
            return subprocess.run(command, capture_output=True, text=True, timeout=10).stdout
        except (OSError, subprocess.SubprocessError):
            continue
    return ""


def discover():
    """A board at 192.168.N.1 gives this computer the address 192.168.N.2."""
    return sorted({f"192.168.{n}.1" for n in HOST_SIDE.findall(interfaces())})


def get(host, path, timeout=20):
    """The bytes at path. Raises OSError with the board's reason on a refusal."""
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        reason = e.read().decode(errors="replace").strip()[:200]
        raise OSError(f"HTTP {e.code} {reason}") from None


class Collection:
    """One board's entries in the zip, and the record of how each went."""

    def __init__(self, archive, host, folder):
        self.archive = archive
        self.host = host
        self.folder = folder
        self.fetched = []
        self.failed = []

    def store(self, name, data):
        self.archive.writestr(f"{self.folder}/{name}", data)
        self.fetched.append({"name": name, "bytes": len(data)})

    def fetch(self, name, path, timeout=20):
        try:
            data = get(self.host, path, timeout)
        except (OSError, ValueError) as e:
            self.failed.append({"name": name, "path": path, "why": str(e)})
            return None
        self.store(name, data)
        return data

    def statuses(self):
        for i in range(1, STATUS_READS + 1):
            self.fetch(f"status_{i}.json", "/api/status")
            if i < STATUS_READS:
                time.sleep(STATUS_GAP_S)

    def files(self):
        listing = self.fetch("fs.json", "/api/fs")
        try:
            entries = json.loads(listing)["files"] if listing else []
        except (ValueError, KeyError, TypeError):
            entries = []
        for entry in entries:
            path = str(entry.get("path", ""))
            if not path.startswith("/") or path.startswith("/www/"):
                continue
            self.fetch("files" + path, path, timeout=120)

    def pressure_trace(self):
        since = 0
        end = time.monotonic() + TRACE_S
        part = 0
        while time.monotonic() < end:
            try:
                data = get(self.host, f"/api/pressure/trace?since={since}", timeout=5)
            except (OSError, ValueError) as e:
                self.failed.append({"name": "pressure_trace", "path": "/api/pressure/trace", "why": str(e)})
                return
            if data[:4] != b"PTR1" or len(data) < 12:
                self.failed.append({"name": "pressure_trace", "path": "/api/pressure/trace", "why": "not a trace"})
                return
            since = int.from_bytes(data[8:12], "little")
            self.store(f"pressure_trace/part_{part:03d}.bin", data)
            part += 1
            time.sleep(0.5)


def collect(archive, host, note):
    """Everything from one board. Returns its manifest entry."""
    try:
        status = json.loads(get(host, "/api/status", timeout=10))
    except (OSError, ValueError) as e:
        print(f"  {host}: no answer ({e})")
        return {"host": host, "answered": False, "why": str(e)}
    serial = re.sub(r"[^A-Za-z0-9]", "", str(status.get("serial", ""))) or host.replace(".", "_")
    print(f"  {host}: {status.get('board', 'board')} {serial}, firmware {status.get('fw_version', '?')}, "
          f"state {status.get('state', '?')}")
    c = Collection(archive, host, serial)
    c.statuses()
    for name, path in JSON_PAGES.items():
        if name != "fs.json":
            c.fetch(name, path)
    c.fetch("flight.csv", "/api/flight.csv", timeout=120)
    c.files()
    c.pressure_trace()
    c.fetch("status_end.json", "/api/status")
    entry = {
        "host": host,
        "answered": True,
        "serial": serial,
        "board": status.get("board"),
        "firmware": status.get("fw_version"),
        "state": status.get("state"),
        "faults": status.get("faults"),
        "note": note,
        "fetched": c.fetched,
        "failed": c.failed,
    }
    archive.writestr(f"{serial}/manifest.json", json.dumps(entry, indent=2))
    print(f"    {len(c.fetched)} items collected, {len(c.failed)} not available")
    return entry


def main():
    ap = argparse.ArgumentParser(description="Collect a Pyro board's data into a zip file to e-mail.")
    ap.add_argument("--host", action="append", help="the board's address, e.g. 192.168.58.1; found by itself if omitted")
    ap.add_argument("--note", default="", help="what you saw or heard, in your own words")
    ap.add_argument("--out", help="the zip file to write; named by the time if omitted")
    a = ap.parse_args()

    hosts = a.host or discover()
    if not hosts:
        print("No board found. Plug the board in by USB, wait ten seconds and run this again.")
        print("If it is plugged in, open its web page, read the address from the browser")
        print("and give it:  python collect_board_data.py --host 192.168.N.1")
        return 1

    stamp = time.strftime("%Y%m%d_%H%M%S", time.gmtime())
    out = a.out or f"pyro_data_{stamp}.zip"
    buffer = io.BytesIO()
    print(f"Collecting from {len(hosts)} board(s); this takes about a minute each.")
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        boards = [collect(archive, host, a.note) for host in hosts]
        archive.writestr("collection.json", json.dumps({
            "collected_utc": stamp,
            "note": a.note,
            "system": platform.system(),
            "python": platform.python_version(),
            "boards": [{k: b.get(k) for k in ("host", "answered", "serial", "firmware", "state")} for b in boards],
        }, indent=2))
    if not any(b["answered"] for b in boards):
        print("No board answered, so no file was written.")
        return 1
    with open(out, "wb") as f:
        f.write(buffer.getvalue())
    print(f"\nWrote {out} ({len(buffer.getvalue()) // 1024} kB). Please e-mail that file.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
