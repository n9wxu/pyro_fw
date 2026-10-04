#!/usr/bin/env python3
"""Install firmware and web files on a Pyro board.

Run from a checkout, or from the release's pyro-support.zip with the board's
images beside it:

  python3 support/install.py [--board mk1c] [--host 192.168.N.1]

The board is the one the device reports on /api/status; with no device
answering, --board names it, and a --board that disagrees with the device is
refused. Images are the release's fw_<board>.uf2, fw_<board>_fota.bin and
fw_<board>_bootloader.uf2, or a local build's pyro_fw_<board>.uf2 and the
pyro_fw_c_fota_image.bin beside it. Every board is an RP2040, so another
board's image installs and runs, with the wrong pin map.

Exits non-zero when anything did not install.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pyro_http import BoardError, board_id_of, find_picotool, get_json, ping, post, wait_down, wait_up  # noqa: E402

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WWW_DIR = os.path.join(BASE_DIR, "www") if os.path.isdir(os.path.join(BASE_DIR, "www")) else None
VERSION_FILE = os.path.join(BASE_DIR, "VERSION")
VERSION = open(VERSION_FILE).read().strip() if os.path.exists(VERSION_FILE) else None


def newest(pattern):
    matches = glob.glob(os.path.join(BASE_DIR, "**", pattern), recursive=True)
    return max(matches, key=os.path.getmtime) if matches else None


def images_for(board):
    """The bootloader, application and OTA images for board, each or None."""
    app = newest(f"fw_{board}.uf2")
    if app:
        return {"bootloader": newest(f"fw_{board}_bootloader.uf2"), "app": app,
                "fota": newest(f"fw_{board}_fota.bin")}
    app = newest(f"pyro_fw_{board}.uf2")
    if not app:
        return {"bootloader": None, "app": None, "fota": None}
    d = os.path.dirname(app)
    fota = os.path.join(d, "pyro_fw_c_fota_image.bin")
    boot = os.path.join(d, "_deps", "pico_fota_bootloader-build", "pico_fota_bootloader.uf2")
    return {"bootloader": boot if os.path.exists(boot) else None, "app": app,
            "fota": fota if os.path.exists(fota) else None}


def run(cmd, timeout=30):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        return "timed out", 1
    except FileNotFoundError:
        return "command not found", 127


def upload_www(host):
    """Every file in www/, then VERSION as /www/version.txt, which the page
    shows as the web files' version. Returns how many failed."""
    files = sorted(f for f in glob.glob(os.path.join(WWW_DIR, "*")) if os.path.isfile(f))
    uploads = [(os.path.basename(f), open(f, "rb").read()) for f in files]
    if VERSION:
        uploads.append(("version.txt", (VERSION + "\n").encode()))
    failed = 0
    for name, data in uploads:
        try:
            post(host, f"/www/{name}", data, timeout=30)
            print(f"    ✓ /www/{name}")
        except BoardError as e:
            print(f"    ✗ /www/{name}: {e}")
            failed += 1
    return failed


def came_back(host, board):
    """The board's status once it answers again, checked against board."""
    print("  Waiting for the board...", flush=True)
    status = wait_up(host, timeout_s=60)
    if not status:
        print(f"  ✗ no answer from {host}")
        return None
    got = board_id_of(status)
    print(f"  up: {got or '?'} v{status.get('fw_version', '?')}")
    if got != board:
        print(f"  ✗ the board reports {got!r}, not {board!r}")
        return None
    return status


def finish(host, board):
    if not came_back(host, board):
        return 1
    if not WWW_DIR:
        print("\n✓ Firmware installed (no web files found).")
        return 0
    print("\nUploading web files...")
    if upload_www(host):
        print("\n✗ Some web files did not upload.")
        return 1
    print("\n✓ Installation complete!")
    return 0


def flash_bootsel(images):
    print("\n1. Hold BOOTSEL and plug in USB")
    input("   Press Enter when the Pico drive appears...")
    drive = next((m for d in ["/Volumes/RPI-RP2", "/media/*/RPI-RP2", "/mnt/*/RPI-RP2"] for m in glob.glob(d)), None)
    drive = drive or input("   Enter Pico drive path: ").strip()
    if not os.path.isdir(drive):
        print(f"Error: {drive} not found")
        return False
    print(f"\n2. Copying the bootloader to {drive}...")
    shutil.copy2(images["bootloader"], drive)
    print("\n3. Hold BOOTSEL and plug in USB again")
    input("   Press Enter when the Pico drive appears...")
    print(f"\n4. Copying the application to {drive}...")
    shutil.copy2(images["app"], drive)
    return True


def flash_picotool(images):
    picotool = find_picotool()
    if not picotool:
        print("Error: picotool not found on PATH or under ~/.pico-sdk")
        return False
    print("\nFlashing via picotool...")
    out, rc = run([picotool, "reboot", "-u", "-f", "--vid", "0x2E8A", "--pid", "0x4002"])
    if rc != 0:
        input("  picotool could not reach the board — hold BOOTSEL, plug in USB, then press Enter...")
    else:
        time.sleep(2)
    for what in ("bootloader", "app"):
        print(f"  Loading the {what}...")
        out, rc = run([picotool, "load", images[what]])
        if rc != 0:
            print(f"  Error: {out}")
            return False
    run([picotool, "reboot"])
    return True


def flash_ota(host, images):
    print("\nUploading firmware via OTA...")
    try:
        print("  " + post(host, "/api/ota", open(images["fota"], "rb").read(), timeout=120).strip())
    except BoardError as e:
        print(f"  ✗ the board refused the image: {e}")
        return False
    if not wait_down(host, timeout_s=20):
        print("  ✗ the board did not restart")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="Install firmware and web files on a Pyro board")
    ap.add_argument("--host", default="192.168.7.1", help="the board's address (192.168.N.1 or pyro.local)")
    ap.add_argument("--board", help="mk1a, mk1b, mk1c...; required when no board answers")
    args = ap.parse_args()

    print("=" * 50)
    print(f"  Pyro installer v{VERSION or 'unknown'}")
    print("=" * 50)

    device_up = ping(args.host)
    status = get_json(args.host) if device_up else None
    reported = board_id_of(status)
    if args.board and reported and args.board != reported:
        print(f"Refused: the board at {args.host} is {reported}, not {args.board}.")
        return 1
    board = args.board or reported
    if not board:
        print(f"No board answers at {args.host}: name it with --board.")
        return 1

    images = images_for(board)
    print(f"\nBoard: {board}" + (f" at {args.host}, v{status.get('fw_version', '?')}" if status else ""))
    print(f"  Bootloader: {images['bootloader'] or 'NOT FOUND'}")
    print(f"  App (UF2):  {images['app'] or 'NOT FOUND'}")
    print(f"  App (OTA):  {images['fota'] or 'NOT FOUND'}")
    print(f"  Web files:  {WWW_DIR or 'NOT FOUND'}\n")

    if device_up:
        print("  1. OTA update (firmware + web files)\n  2. Web files only\n"
              "  3. Full flash via picotool (bootloader + app + web)\n  4. Exit")
        choice = {"1": "ota", "2": "www", "3": "picotool"}.get(input("\nSelect [1]: ").strip() or "1")
    else:
        print(f"No board answers at {args.host}.")
        print("  1. Full flash via BOOTSEL (hold BOOTSEL, plug USB)\n  2. Full flash via picotool\n  3. Exit")
        choice = {"1": "bootsel", "2": "picotool"}.get(input("\nSelect [1]: ").strip() or "1")
    if not choice:
        return 0

    if choice == "www":
        if not WWW_DIR:
            print("Error: www directory not found")
            return 1
        return 1 if upload_www(args.host) else 0

    need = ["fota"] if choice == "ota" else ["bootloader", "app"]
    missing = [k for k in need if not images[k]]
    if missing:
        print(f"Error: no {', '.join(missing)} image for {board}")
        return 1
    flashed = {"ota": lambda: flash_ota(args.host, images), "bootsel": lambda: flash_bootsel(images),
               "picotool": lambda: flash_picotool(images)}[choice]()
    if not flashed:
        return 1
    return finish(args.host, board)


if __name__ == "__main__":
    sys.exit(main())
