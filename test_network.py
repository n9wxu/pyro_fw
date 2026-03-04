#!/usr/bin/env python3
"""Comprehensive network test for Pyro MK1B.

Usage: ./test_network.py [options] [host]

Options:
  --all           Run all tests even if one fails
  --uart PORT     Monitor UART during tests (e.g. /dev/tty.usbmodem201202)
  --wait-boot     Wait for device boot before testing (use with --uart)
  host            Default: pyro.local
"""
import subprocess, sys, time, os, signal, argparse, threading, io

parser = argparse.ArgumentParser(description="Pyro MK1B network test")
parser.add_argument("host", nargs="?", default="pyro.local")
parser.add_argument("--all", action="store_true", help="Run all tests even on failure")
parser.add_argument("--uart", metavar="PORT", help="Monitor UART port during tests")
parser.add_argument("--wait-boot", action="store_true", help="Wait for boot messages before testing")
args = parser.parse_args()

passed = 0
failed = 0
uart_log = []
uart_proc = None

# --- UART monitoring ---

def start_uart(port):
    global uart_proc
    os.system(f"stty -f {port} 115200 raw -echo 2>/dev/null")
    uart_proc = subprocess.Popen(
        ["cat", port], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def reader():
        for line in io.TextIOWrapper(uart_proc.stdout, errors="replace"):
            uart_log.append(line.rstrip())
    t = threading.Thread(target=reader, daemon=True)
    t.start()

def stop_uart():
    global uart_proc
    if uart_proc:
        uart_proc.kill()
        uart_proc.wait()

def wait_for_boot():
    print("Waiting for boot (press reset now)...")
    deadline = time.time() + 30
    while time.time() < deadline:
        if any("entering main loop" in l for l in uart_log):
            print("  Boot complete.")
            time.sleep(2)  # let network settle
            return
        time.sleep(0.2)
    print("  Timeout waiting for boot.")

# --- Test helpers ---

def run(cmd, timeout=5):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        return "", 1

def check(desc, ok):
    global passed, failed
    if ok:
        print(f"  ✓ {desc}")
        passed += 1
    else:
        print(f"  ✗ {desc}")
        failed += 1
        if not args.all:
            dump_results()
            sys.exit(1)

def curl(path, timeout=5):
    out, _ = run(["curl", "-s", "--max-time", str(timeout), f"http://{args.host}{path}"])
    return out

def dump_results():
    if uart_log:
        print(f"\n=== UART LOG ({len(uart_log)} lines) ===")
        for line in uart_log:
            print(f"  {line}")
    print(f"\n=== Results: {passed} passed, {failed} failed ===")

# --- Main ---

if args.uart:
    start_uart(args.uart)
    if args.wait_boot:
        wait_for_boot()
    else:
        time.sleep(1)

print(f"=== Pyro MK1B Network Test ===")
print(f"Host: {args.host}  Mode: {'all' if args.all else 'fail-fast'}")
if args.uart:
    print(f"UART: {args.uart}")
print()

try:
    # Ping
    print("-- Connectivity --")
    out, rc = run(["ping", "-c", "1", "-t", "3", args.host])
    check("ping", rc == 0)

    # HTTP API
    print("\n-- HTTP API --")
    s = curl("/api/status")
    check("GET /api/status returns JSON", "fw_version" in s)
    check("status has state", "PAD_IDLE" in s or "ASCENT" in s or "DESCENT" in s or "LANDED" in s)
    check("status has pressure", "pressure_pa" in s)

    c = curl("/api/config")
    check("GET /api/config returns INI", "[pyro]" in c)

    out, _ = run(["curl", "-s", "-D-", "--max-time", "5", f"http://{args.host}/api/status"])
    check("CORS header present", "Access-Control-Allow-Origin" in out)

    # File serving
    print("\n-- File serving --")
    idx = curl("/")
    check(f"GET / loads index.html ({len(idx)}b)", len(idx) > 900)
    js = curl("/www/app.js")
    check(f"GET /www/app.js ({len(js)}b)", len(js) > 2000)
    css = curl("/www/style.css")
    check(f"GET /www/style.css ({len(css)}b)", len(css) > 300)

    # Parallel
    print("\n-- Parallel connections --")
    procs = []
    for i in range(6):
        p = subprocess.Popen(
            ["curl", "-s", "--max-time", "5", f"http://{args.host}/api/status"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        procs.append(p)
    results = [p.communicate()[0].decode() for p in procs]
    ok = sum(1 for r in results if "fw_version" in r)
    check(f"6 parallel requests ({ok}/6 succeeded)", ok == 6)

    # Sequential
    print("\n-- Rapid sequential --")
    ok = 0
    for i in range(10):
        s = curl("/api/status", timeout=3)
        if "fw_version" in s:
            ok += 1
    check(f"10 sequential requests ({ok}/10)", ok == 10)

    # mDNS
    print("\n-- mDNS/DNS-SD --")
    p = subprocess.Popen(
        ["dns-sd", "-B", "_pyro._tcp", "local."],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(3)
    p.kill()
    out = p.communicate()[0].decode()
    check("_pyro._tcp service found", "pyro" in out)

finally:
    stop_uart()
    dump_results()

sys.exit(1 if failed else 0)
