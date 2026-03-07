#!/usr/bin/env python3
"""Pyro MK1B Installer — flash firmware and upload web files.

Run from the downloaded artifact directory:
  python3 install.py
"""
import subprocess, sys, os, time, glob

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Find files relative to script location
def find(pattern):
    matches = glob.glob(os.path.join(SCRIPT_DIR, "**", pattern), recursive=True)
    return matches[0] if matches else None

BOOTLOADER = find("pico_fota_bootloader.uf2")
APP_UF2 = find("pyro_fw_c.uf2")
FOTA_BIN = find("pyro_fw_c_fota_image.bin")
WWW_DIR = find("www")
if WWW_DIR and not os.path.isdir(WWW_DIR):
    WWW_DIR = os.path.dirname(WWW_DIR)
else:
    for d in [os.path.join(SCRIPT_DIR, "www"), os.path.join(SCRIPT_DIR, "..", "www")]:
        if os.path.isdir(d):
            WWW_DIR = d
            break

VERSION = "unknown"
vf = find("VERSION")
if vf:
    VERSION = open(vf).read().strip()

HOST = "192.168.7.1"

def run(cmd, timeout=30):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        return "", 1
    except FileNotFoundError:
        return "command not found", 127

def ping(host):
    _, rc = run(["ping", "-c", "1", "-t", "2", host])
    return rc == 0

def curl_post(url, filepath):
    with open(filepath, "rb") as f:
        data = f.read()
    try:
        import urllib.request
        req = urllib.request.Request(url, data=data,
            headers={"Content-Type": "application/octet-stream"}, method="POST")
        urllib.request.urlopen(req, timeout=120)
        return True
    except:
        return True  # device reboots mid-response

def upload_www(host, www_dir):
    ok = 0
    for f in sorted(glob.glob(os.path.join(www_dir, "*"))):
        if os.path.isfile(f):
            name = os.path.basename(f)
            try:
                with open(f, "rb") as fh:
                    data = fh.read()
                import urllib.request
                req = urllib.request.Request(f"http://{host}/www/{name}",
                    data=data, method="POST")
                urllib.request.urlopen(req, timeout=10)
                print(f"    ✓ /www/{name}")
                ok += 1
            except Exception as e:
                print(f"    ✗ /www/{name}: {e}")
    return ok

def wait_for_device(host, timeout=20):
    print(f"  Waiting for device...", end="", flush=True)
    for i in range(timeout):
        time.sleep(1)
        print(".", end="", flush=True)
        if ping(host):
            print(f" up!")
            return True
    print(" timeout")
    return False

def main():
    print("=" * 50)
    print(f"  Pyro MK1B Installer v{VERSION}")
    print("=" * 50)
    print()

    # Show what we found
    print("Files found:")
    print(f"  Bootloader: {BOOTLOADER or 'NOT FOUND'}")
    print(f"  App (UF2):  {APP_UF2 or 'NOT FOUND'}")
    print(f"  App (OTA):  {FOTA_BIN or 'NOT FOUND'}")
    print(f"  Web files:  {WWW_DIR or 'NOT FOUND'}")
    print()

    # Check if device is already running
    device_up = ping(HOST)

    if device_up:
        print(f"Device found at {HOST}")
        print()
        print("Options:")
        print("  1. OTA update (firmware + web files)")
        print("  2. Web files only")
        print("  3. Full flash via picotool (bootloader + app + web)")
        print("  4. Exit")
        choice = input("\nSelect [1]: ").strip() or "1"
    else:
        print(f"No device found at {HOST}")
        print()
        print("Options:")
        print("  1. Full flash via BOOTSEL (hold BOOTSEL button, plug USB)")
        print("  2. Full flash via picotool")
        print("  3. Exit")
        choice = input("\nSelect [1]: ").strip() or "1"

        if choice == "3":
            return
        if choice == "1":
            if not BOOTLOADER or not APP_UF2:
                print("Error: UF2 files not found"); return
            print("\n1. Hold BOOTSEL and plug in USB")
            input("   Press Enter when the Pico drive appears...")
            # Find mounted Pico drive
            pico_drive = None
            for d in ["/Volumes/RPI-RP2", "/media/*/RPI-RP2", "/mnt/*/RPI-RP2"]:
                matches = glob.glob(d)
                if matches:
                    pico_drive = matches[0]
                    break
            if not pico_drive:
                pico_drive = input("   Enter Pico drive path: ").strip()
            if not os.path.isdir(pico_drive):
                print(f"Error: {pico_drive} not found"); return

            print(f"\n2. Copying bootloader to {pico_drive}...")
            import shutil
            shutil.copy2(BOOTLOADER, pico_drive)
            print("   Done. Device will reboot.")
            print("\n3. Hold BOOTSEL and plug in USB again")
            input("   Press Enter when the Pico drive appears...")

            print(f"\n4. Copying application to {pico_drive}...")
            shutil.copy2(APP_UF2, pico_drive)
            print("   Done. Device will reboot.")

            if wait_for_device(HOST) and WWW_DIR:
                print("\n5. Uploading web files...")
                upload_www(HOST, WWW_DIR)
            print("\n✓ Installation complete!")
            return

        # choice == "2" falls through to picotool below
        choice = "3"

    # OTA update
    if choice == "1" and device_up:
        if not FOTA_BIN:
            print("Error: OTA binary not found"); return
        print(f"\nUploading firmware via OTA...")
        curl_post(f"http://{HOST}/api/ota", FOTA_BIN)
        print("  Firmware uploaded, device rebooting...")
        time.sleep(3)
        if wait_for_device(HOST) and WWW_DIR:
            print("\nUploading web files...")
            upload_www(HOST, WWW_DIR)
        print("\n✓ Update complete!")
        return

    # Web files only
    if choice == "2" and device_up:
        if not WWW_DIR:
            print("Error: www directory not found"); return
        print("\nUploading web files...")
        upload_www(HOST, WWW_DIR)
        print("\n✓ Web files updated!")
        return

    # Picotool flash
    if choice == "3":
        picotool = None
        for p in ["picotool", os.path.expanduser("~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool")]:
            out, rc = run([p, "version"])
            if rc != 127:
                picotool = p
                break
        if not picotool:
            print("Error: picotool not found"); return
        if not BOOTLOADER or not APP_UF2:
            print("Error: UF2 files not found"); return

        print(f"\nFlashing via picotool...")
        if device_up:
            print("  Forcing BOOTSEL...")
            run([picotool, "reboot", "-u", "-f", "--vid", "0x2E8A", "--pid", "0x4002"])
            time.sleep(2)

        print("  Loading bootloader...")
        out, rc = run([picotool, "load", BOOTLOADER])
        if rc != 0:
            print(f"  Error: {out}"); return

        print("  Loading application...")
        out, rc = run([picotool, "load", APP_UF2])
        if rc != 0:
            print(f"  Error: {out}"); return

        print("  Rebooting...")
        run([picotool, "reboot"])

        if wait_for_device(HOST) and WWW_DIR:
            print("\nUploading web files...")
            upload_www(HOST, WWW_DIR)
        print("\n✓ Installation complete!")
        return

    if choice == "4":
        return

    print("Invalid selection")

if __name__ == "__main__":
    main()
