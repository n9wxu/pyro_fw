#!/usr/bin/env python3
"""Comprehensive network test for Pyro MK1B.

Usage: ./test_network.py [options] [host]

Options:
  --all           Run all tests even if one fails
  --uart PORT     Monitor UART during tests (e.g. /dev/tty.usbmodem201202)
  --reset         Use picotool to reset device before testing
  --repeat N      Repeat the full test suite N times
  host            Default: pyro.local
"""
import subprocess, sys, time, os, argparse, threading, io, json

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

PICOTOOL = os.path.expanduser("~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool")

parser = argparse.ArgumentParser(description="Pyro MK1B network test")
parser.add_argument("host", nargs="?", default="pyro.local")
parser.add_argument("--all", action="store_true", help="Run all tests even on failure")
parser.add_argument("--uart", metavar="PORT", help="Monitor UART port during tests")
parser.add_argument("--reset", action="store_true", help="Reset device via picotool before testing")
parser.add_argument("--repeat", type=int, default=1, help="Repeat test suite N times")
args = parser.parse_args()

passed = 0
failed = 0
uart_log = []

# --- UART monitoring via pyserial ---

uart_ser = None

def start_uart(port):
    global uart_ser
    if not HAS_SERIAL:
        print(f"  WARNING: pyserial not installed, UART monitoring disabled")
        return
    try:
        uart_ser = serial.Serial(port, 115200, timeout=0.1)
        uart_ser.reset_input_buffer()
        def reader():
            buf = b""
            while uart_ser and uart_ser.is_open:
                try:
                    data = uart_ser.read(1024)
                    if data:
                        buf += data
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            uart_log.append(line.decode(errors="replace").rstrip())
                except (serial.SerialException, OSError):
                    break
        t = threading.Thread(target=reader, daemon=True)
        t.start()
        print(f"  UART monitoring started on {port}")
    except serial.SerialException as e:
        print(f"  WARNING: Could not open {port}: {e}")

def stop_uart():
    global uart_ser
    if uart_ser and uart_ser.is_open:
        uart_ser.close()
    uart_ser = None

# --- picotool ---

def picotool_reset():
    """Reset device via picotool and wait for network."""
    print("Resetting device via picotool...")
    # Force into BOOTSEL then back to app
    subprocess.run([PICOTOOL, "reboot", "-u", "-f", "--vid", "0x2E8A", "--pid", "0x4002"],
                   capture_output=True, timeout=10)
    time.sleep(1)
    subprocess.run([PICOTOOL, "reboot"], capture_output=True, timeout=10)
    print("  Waiting for network...")
    for i in range(20):
        time.sleep(1)
        r = subprocess.run(["ping", "-c", "1", "-t", "2", args.host],
                           capture_output=True)
        if r.returncode == 0:
            print(f"  Device up after {i+1}s")
            time.sleep(1)  # extra settle
            return True
    print("  Timeout waiting for device")
    return False

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

def curl_headers(path, timeout=5):
    out, _ = run(["curl", "-s", "-D-", "--max-time", str(timeout), f"http://{args.host}{path}"])
    return out

def curl_size(path, timeout=5):
    body = curl(path, timeout)
    return body, len(body)

def dump_results():
    if uart_log:
        print(f"\n=== UART LOG ({len(uart_log)} lines) ===")
        for line in uart_log[-50:]:  # last 50 lines
            print(f"  {line}")
    print(f"\n=== Results: {passed} passed, {failed} failed ===")

# --- Test suite ---

def run_tests():
    global passed, failed

    # Connectivity
    print("-- Connectivity --")
    out, rc = run(["ping", "-c", "1", "-t", "3", args.host])
    check("ping", rc == 0)

    # HTTP API
    print("\n-- HTTP API --")
    s = curl("/api/status")
    check("GET /api/status returns JSON", "fw_version" in s)

    try:
        d = json.loads(s)
        check("status has state", d.get("state") in ("PAD_IDLE", "ASCENT", "DESCENT", "LANDED"))
        check("status has pressure_pa", "pressure_pa" in d)
        check("status has fw_version", "fw_version" in d)
        check("status has uptime", "uptime" in d)
        check("status has pyro1_cont", "pyro1_cont" in d)
    except (json.JSONDecodeError, TypeError):
        check("status is valid JSON", False)

    c = curl("/api/config")
    check("GET /api/config returns INI", "[pyro]" in c)

    h = curl_headers("/api/status")
    check("CORS header present", "Access-Control-Allow-Origin" in h)

    # File serving - size checks
    print("\n-- File serving --")
    idx, idx_sz = curl_size("/")
    check(f"GET / complete ({idx_sz}b)", idx_sz > 900)
    check("index.html has doctype", "<!DOCTYPE" in idx)

    js, js_sz = curl_size("/www/app.js")
    check(f"GET /www/app.js complete ({js_sz}b)", js_sz > 2000)
    check("app.js has update function", "function update" in js)

    css, css_sz = curl_size("/www/style.css")
    check(f"GET /www/style.css complete ({css_sz}b)", css_sz > 300)

    # File consistency - fetch same file multiple times
    print("\n-- File consistency --")
    sizes = []
    for i in range(5):
        _, sz = curl_size("/www/app.js", timeout=5)
        sizes.append(sz)
    all_same = len(set(sizes)) == 1 and sizes[0] > 2000
    check(f"app.js 5x consistent (sizes: {sizes})", all_same)

    sizes = []
    for i in range(5):
        _, sz = curl_size("/", timeout=5)
        sizes.append(sz)
    all_same = len(set(sizes)) == 1 and sizes[0] > 900
    check(f"index.html 5x consistent (sizes: {sizes})", all_same)

    # 404
    print("\n-- Error handling --")
    r404 = curl("/nonexistent")
    check("404 for missing file", "Not found" in r404 or "404" in r404)

    # Parallel connections
    print("\n-- Parallel connections --")
    procs = []
    for i in range(6):
        p = subprocess.Popen(
            ["curl", "-s", "--max-time", "5", f"http://{args.host}/api/status"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        procs.append(p)
    results = [p.communicate()[0].decode() for p in procs]
    ok = sum(1 for r in results if "fw_version" in r)
    check(f"6 parallel status requests ({ok}/6)", ok == 6)

    # Parallel file requests
    procs = []
    for path in ["/", "/www/app.js", "/www/style.css", "/api/status", "/api/config", "/"]:
        p = subprocess.Popen(
            ["curl", "-s", "--max-time", "5", f"http://{args.host}{path}"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        procs.append(p)
    results = [len(p.communicate()[0]) for p in procs]
    check(f"6 parallel mixed requests (sizes: {results})", all(s > 0 for s in results))

    # Rapid sequential
    print("\n-- Rapid sequential --")
    ok = 0
    for i in range(20):
        s = curl("/api/status", timeout=3)
        if "fw_version" in s:
            ok += 1
    check(f"20 sequential status requests ({ok}/20)", ok == 20)

    # Sustained file serving
    print("\n-- Sustained file serving --")
    ok = 0
    for i in range(10):
        _, sz = curl_size("/www/app.js", timeout=5)
        if sz > 2000:
            ok += 1
    check(f"10 sequential file downloads ({ok}/10)", ok == 10)

    # mDNS/DNS-SD
    print("\n-- mDNS/DNS-SD --")
    p = subprocess.Popen(
        ["dns-sd", "-B", "_pyro._tcp", "local."],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(3)
    p.kill()
    out = p.communicate()[0].decode()
    check("_pyro._tcp service found", "pyro" in out)

    # picotool
    print("\n-- picotool --")
    out, rc = run([PICOTOOL, "reboot", "-f", "--vid", "0x2E8A", "--pid", "0x4002"], timeout=10)
    # -f on a running device with vendor reset should work
    check("picotool sees device", "reboot" in out.lower() or rc == 0 or "RP-series" in out)

# --- Main ---

if args.uart:
    start_uart(args.uart)
    time.sleep(1)

try:
    for iteration in range(args.repeat):
        if args.repeat > 1:
            print(f"\n{'='*50}")
            print(f"=== Iteration {iteration + 1}/{args.repeat} ===")
            print(f"{'='*50}\n")

        if args.reset or (iteration > 0):
            if not picotool_reset():
                check("device reset", False)
                continue

        print(f"=== Pyro MK1B Network Test ===")
        print(f"Host: {args.host}  Mode: {'all' if args.all else 'fail-fast'}")
        if args.uart:
            print(f"UART: {args.uart}")
        print()

        run_tests()

finally:
    stop_uart()
    dump_results()

sys.exit(1 if failed else 0)
