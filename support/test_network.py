#!/usr/bin/env python3
"""Comprehensive network test for Pyro MK1B with live TUI.

Usage:
  ./test_network.py                     Interactive mode
  ./test_network.py [options] [host]    Direct mode
  ./test_network.py --analyze FILE      Analyze a log file

Options:
  --all           Run all tests even if one fails
  --uart PORT     Monitor UART during tests
  --reset         Reset device via picotool before testing
  --repeat N      Repeat test suite N times
  --plain         Disable TUI, plain text output
  --log FILE      Write timestamped log to file
  --analyze FILE  Analyze a previous log file
  host            Default: pyro.local
"""
import subprocess, sys, time, os, argparse, threading, json, curses, glob

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

PICOTOOL = os.path.expanduser("~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool")

parser = argparse.ArgumentParser(description="Pyro MK1B network test")
parser.add_argument("host", nargs="?", default=None)
parser.add_argument("--all", action="store_true")
parser.add_argument("--uart", metavar="PORT")
parser.add_argument("--reset", action="store_true")
parser.add_argument("--repeat", type=int, default=1)
parser.add_argument("--plain", action="store_true")
parser.add_argument("--log", metavar="FILE")
parser.add_argument("--analyze", metavar="FILE")
args = parser.parse_args()

# ── Timeline log ──

log_entries = []
log_lock = threading.Lock()
log_file = None
t0 = time.time()
screen_dirty = threading.Event()

def log(source, msg):
    ts = time.time() - t0
    with log_lock:
        log_entries.append((ts, source, msg))
    if log_file:
        log_file.write(f"[{ts:8.3f}] [{source:5s}] {msg}\n")
        log_file.flush()
    screen_dirty.set()

# ── Test registry ──

PENDING, RUNNING, PASSED, FAILED = 0, 1, 2, 3
tests = []
uart_lines = []
uart_error = None
ui_lock = threading.Lock()

def add_section(name):
    with ui_lock:
        tests.append((name, None, PENDING, ""))

def add_test(name):
    with ui_lock:
        tests.append((None, name, PENDING, ""))
    return len(tests) - 1

def set_running(idx):
    with ui_lock:
        s, n, _, d = tests[idx]
        tests[idx] = (s, n, RUNNING, d)
    log("TEST", f"START {tests[idx][1]}")

def set_result(idx, ok, detail=""):
    with ui_lock:
        s, n, _, _ = tests[idx]
        tests[idx] = (s, n, PASSED if ok else FAILED, detail)
    log("TEST", f"{'PASS' if ok else 'FAIL'} {tests[idx][1]} {detail}")

# ── UART via pyserial ──

uart_ser = None

def start_uart(port):
    global uart_ser, uart_error
    if not HAS_SERIAL:
        uart_error = "pyserial not installed (pip install pyserial)"
        log("UART", f"ERROR: {uart_error}")
        return
    try:
        uart_ser = serial.Serial(port, 115200, timeout=0.1)
        uart_ser.reset_input_buffer()
        log("UART", f"Opened {port}")
        def reader():
            buf = b""
            while uart_ser and uart_ser.is_open:
                try:
                    data = uart_ser.read(1024)
                    if data:
                        buf += data
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            text = line.decode(errors="replace").rstrip()
                            with ui_lock:
                                uart_lines.append(text)
                                if len(uart_lines) > 500:
                                    del uart_lines[:100]
                            log("UART", text)
                except (serial.SerialException, OSError):
                    break
        t = threading.Thread(target=reader, daemon=True)
        t.start()
    except serial.SerialException as e:
        uart_error = str(e)
        log("UART", f"ERROR: {uart_error}")

def stop_uart():
    global uart_ser
    if uart_ser and uart_ser.is_open:
        uart_ser.close()
    uart_ser = None

# ── picotool ──

def picotool_reset(host):
    log("CMD", "picotool reboot -u -f")
    subprocess.run([PICOTOOL, "reboot", "-u", "-f", "--vid", "0x2E8A", "--pid", "0x4002"],
                   capture_output=True, timeout=10)
    time.sleep(1)
    log("CMD", "picotool reboot")
    subprocess.run([PICOTOOL, "reboot"], capture_output=True, timeout=10)
    for i in range(20):
        time.sleep(1)
        r = subprocess.run(["ping", "-c", "1", "-t", "2", host], capture_output=True)
        if r.returncode == 0:
            log("CMD", f"Device up after {i+1}s")
            time.sleep(1)
            return True
    log("CMD", "Device reset timeout")
    return False

# ── Test helpers ──

def run_cmd(cmd, timeout=5):
    t_start = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr, r.returncode, time.time() - t_start
    except subprocess.TimeoutExpired:
        return "", 1, time.time() - t_start

def curl(host, path, timeout=5):
    log("CMD", f"GET http://{host}{path}")
    out, rc, elapsed = run_cmd(
        ["curl", "-s", "-w", "\n%{http_code}", "--max-time", str(timeout),
         f"http://{host}{path}"])
    parts = out.rsplit("\n", 1)
    body = parts[0] if len(parts) > 1 else out
    http_code = parts[1] if len(parts) > 1 else "?"
    preview = body[:200].replace("\n", "\\n") if body else "(empty)"
    log("RESP", f"{http_code} {len(body)}b {elapsed:.3f}s {preview}")
    if rc != 0 or http_code == "000":
        log("RESP", f"FULL BODY: {body[:2000]}")
    return body

def curl_headers(host, path, timeout=5):
    log("CMD", f"GET -D- http://{host}{path}")
    out, _, elapsed = run_cmd(
        ["curl", "-s", "-D-", "--max-time", str(timeout), f"http://{host}{path}"])
    hdr_end = out.find("\r\n\r\n")
    if hdr_end > 0:
        log("RESP", f"HEADERS: {out[:hdr_end].replace(chr(13), '')}")
    return out

# ── Test definitions ──

def define_tests():
    plan = []

    add_section("Connectivity")
    idx = add_test("ping")
    def t_ping(h):
        _, rc, _ = run_cmd(["ping", "-c", "1", "-t", "3", h])
        return rc == 0, ""
    plan.append((idx, t_ping))

    add_section("HTTP API")
    idx = add_test("GET /api/status returns JSON")
    def t_status(h):
        s = curl(h, "/api/status")
        return "fw_version" in s, f"{len(s)}b"
    plan.append((idx, t_status))

    idx = add_test("status is valid JSON with fields")
    def t_fields(h):
        s = curl(h, "/api/status")
        try:
            d = json.loads(s)
            ok = all(k in d for k in ("state", "pressure_pa", "fw_version", "uptime", "pyro1_cont"))
            return ok, d.get("fw_version", "?")
        except:
            return False, "invalid JSON"
    plan.append((idx, t_fields))

    idx = add_test("GET /api/config returns INI")
    def t_config(h):
        c = curl(h, "/api/config")
        return "[pyro]" in c, f"{len(c)}b"
    plan.append((idx, t_config))

    idx = add_test("CORS header present")
    def t_cors(h):
        out = curl_headers(h, "/api/status")
        return "Access-Control-Allow-Origin" in out, ""
    plan.append((idx, t_cors))

    add_section("File Serving")
    for name, path, min_sz, check_str in [
        ("GET / (index.html)", "/", 900, "<!DOCTYPE"),
        ("GET /www/app.js", "/www/app.js", 2000, "function update"),
        ("GET /www/style.css", "/www/style.css", 300, None),
    ]:
        idx = add_test(name)
        def make_fn(p, ms, cs):
            def fn(h):
                s = curl(h, p)
                ok = len(s) > ms and (cs is None or cs in s)
                return ok, f"{len(s)}b"
            return fn
        plan.append((idx, make_fn(path, min_sz, check_str)))

    add_section("File Consistency")
    idx = add_test("app.js 5x consistent")
    def t_js5(h):
        sizes = [len(curl(h, "/www/app.js")) for _ in range(5)]
        return len(set(sizes)) == 1 and sizes[0] > 2000, str(sizes)
    plan.append((idx, t_js5))

    idx = add_test("index.html 5x consistent")
    def t_idx5(h):
        sizes = [len(curl(h, "/")) for _ in range(5)]
        return len(set(sizes)) == 1 and sizes[0] > 900, str(sizes)
    plan.append((idx, t_idx5))

    add_section("Error Handling")
    idx = add_test("404 for missing file")
    def t_404(h):
        r = curl(h, "/nonexistent")
        return "Not found" in r or "404" in r, ""
    plan.append((idx, t_404))

    add_section("Parallel Connections")
    idx = add_test("6 parallel status requests")
    def t_par(h):
        log("CMD", "6x parallel /api/status")
        procs = [subprocess.Popen(["curl", "-s", "--max-time", "5", f"http://{h}/api/status"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(6)]
        results = [p.communicate()[0].decode() for p in procs]
        ok = sum(1 for r in results if "fw_version" in r)
        log("CMD", f"  -> {ok}/6 succeeded")
        return ok == 6, f"{ok}/6"
    plan.append((idx, t_par))

    idx = add_test("6 parallel mixed requests")
    def t_parmix(h):
        paths = ["/", "/www/app.js", "/www/style.css", "/api/status", "/api/config", "/"]
        log("CMD", "6x parallel mixed")
        procs = [subprocess.Popen(["curl", "-s", "--max-time", "5", f"http://{h}{p}"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) for p in paths]
        sizes = [len(p.communicate()[0]) for p in procs]
        log("CMD", f"  -> sizes {sizes}")
        return all(s > 0 for s in sizes), str(sizes)
    plan.append((idx, t_parmix))

    add_section("Sequential Requests")
    idx = add_test("20 sequential status requests")
    def t_seq20(h):
        ok = sum(1 for _ in range(20) if "fw_version" in curl(h, "/api/status", 3))
        return ok == 20, f"{ok}/20"
    plan.append((idx, t_seq20))

    idx = add_test("10 sequential file downloads")
    def t_seq10(h):
        ok = sum(1 for _ in range(10) if len(curl(h, "/www/app.js")) > 2000)
        return ok == 10, f"{ok}/10"
    plan.append((idx, t_seq10))

    add_section("mDNS/DNS-SD")
    idx = add_test("_pyro._tcp service found")
    def t_mdns(h):
        log("CMD", "dns-sd -B _pyro._tcp local.")
        p = subprocess.Popen(["dns-sd", "-B", "_pyro._tcp", "local."],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(3)
        p.kill()
        out = p.communicate()[0].decode()
        found = "pyro" in out
        log("CMD", f"  -> {'found' if found else 'not found'}")
        return found, ""
    plan.append((idx, t_mdns))

    add_section("picotool")
    idx = add_test("picotool sees device")
    def t_pico(h):
        log("CMD", "picotool reboot -f")
        out, rc, _ = run_cmd([PICOTOOL, "reboot", "-f", "--vid", "0x2E8A", "--pid", "0x4002"], 10)
        log("RESP", f"rc={rc} {out.strip()}")
        return rc == 0 or "RP-series" in out, ""
    plan.append((idx, t_pico))

    return plan

# ── TUI ──

tui_running = False

def draw_tui(stdscr):
    global tui_running
    curses.curs_set(0)
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_GREEN, -1)
    curses.init_pair(2, curses.COLOR_RED, -1)
    curses.init_pair(3, curses.COLOR_WHITE, -1)
    curses.init_pair(4, 8, -1)
    tui_running = True

    def refresher():
        while tui_running:
            screen_dirty.wait(timeout=0.2)
            screen_dirty.clear()
            try: refresh_screen(stdscr, args.host)
            except: pass
    threading.Thread(target=refresher, daemon=True).start()

    host = args.host
    plan = define_tests()
    if args.uart: start_uart(args.uart)

    if args.reset:
        with ui_lock:
            tests.insert(0, ("Setup", None, PENDING, ""))
            tests.insert(1, (None, "picotool reset", RUNNING, ""))
        log("TEST", "START picotool reset")
        ok = picotool_reset(host)
        with ui_lock:
            tests[1] = (None, "picotool reset", PASSED if ok else FAILED, "")
        log("TEST", f"{'PASS' if ok else 'FAIL'} picotool reset")
        if not ok and not args.all:
            time.sleep(2); tui_running = False; return

    for idx, fn in plan:
        set_running(idx)
        try: ok, detail = fn(host)
        except Exception as e: ok, detail = False, str(e)
        set_result(idx, ok, detail)
        if not ok and not args.all: break

    tui_running = False
    time.sleep(0.2)
    refresh_screen(stdscr, host)
    stdscr.addstr(curses.LINES - 1, 0, "Press any key to exit...")
    stdscr.refresh()
    stdscr.nodelay(False)
    stdscr.getch()

def refresh_screen(stdscr, host):
    stdscr.erase()
    h, w = stdscr.getmaxyx()
    has_uart = args.uart is not None
    left_w = w // 2 if has_uart else w - 1
    right_x = left_w + 1

    p = sum(1 for _, n, s, _ in tests if n and s == PASSED)
    f = sum(1 for _, n, s, _ in tests if n and s == FAILED)
    t = sum(1 for _, n, _, _ in tests if n)
    hdr = f" Pyro MK1B Test — {host} — {p}/{t} passed"
    if f: hdr += f", {f} failed"
    safe_addstr(stdscr, 0, 0, hdr[:w-1], curses.A_BOLD)

    row = 2
    with ui_lock:
        for sec, name, state, detail in tests:
            if row >= h - 2: break
            if sec and not name:
                safe_addstr(stdscr, row, 1, f"── {sec} ──", curses.A_BOLD); row += 1; continue
            if state == PASSED:    sym, attr = "✓", curses.color_pair(1)
            elif state == FAILED:  sym, attr = "✗", curses.color_pair(2)
            elif state == RUNNING: sym, attr = "▸", curses.color_pair(3) | curses.A_BOLD
            else:                  sym, attr = "·", curses.color_pair(4)
            line = f" {sym} {name}"
            if detail and state in (PASSED, FAILED): line += f"  ({detail})"
            safe_addstr(stdscr, row, 1, line[:left_w-2], attr); row += 1

    if has_uart:
        for r in range(h): safe_addstr(stdscr, r, left_w, "│")
        uhdr = " UART"
        if uart_error: uhdr += f" ⚠ {uart_error}"
        safe_addstr(stdscr, 0, right_x, uhdr[:w-right_x-1], curses.A_BOLD)
        with ui_lock:
            visible = uart_lines[-(h-3):]
        for i, line in enumerate(visible):
            r = 2 + i
            if r >= h - 1: break
            safe_addstr(stdscr, r, right_x + 1, line[:w-right_x-2])
    stdscr.refresh()

def safe_addstr(stdscr, y, x, s, attr=0):
    h, w = stdscr.getmaxyx()
    if y < h and x < w:
        try: stdscr.addnstr(y, x, s, w - x - 1, attr)
        except curses.error: pass

# ── Pre-test diagnostics (logged for remote analysis) ──

def collect_diagnostics(host):
    log("DIAG", "=== Pre-test diagnostics ===")

    # mDNS resolution
    log("DIAG", f"Resolving {host}...")
    out, rc, elapsed = run_cmd(["dns-sd", "-G", "v4", host], timeout=5)
    # dns-sd doesn't exit on its own, so it'll timeout — parse what we got
    log("DIAG", f"dns-sd result ({elapsed:.1f}s): {out.strip()[:200]}")

    # Try to get the actual IP
    out2, rc2, _ = run_cmd(["dscacheutil", "-q", "host", "-a", "name", host], timeout=3)
    log("DIAG", f"dscacheutil: {out2.strip()[:200]}")

    # Ping by name
    out3, rc3, elapsed3 = run_cmd(["ping", "-c", "1", "-t", "3", host])
    log("DIAG", f"ping {host}: rc={rc3} ({elapsed3:.1f}s) {out3.strip()[:200]}")

    # If host is a name, also try direct IP
    if not host[0].isdigit():
        out4, rc4, elapsed4 = run_cmd(["ping", "-c", "1", "-t", "3", "192.168.7.1"])
        log("DIAG", f"ping 192.168.7.1: rc={rc4} ({elapsed4:.1f}s)")
        if rc4 == 0:
            out5, _, elapsed5 = run_cmd(
                ["curl", "-s", "-w", "\n%{http_code}", "--max-time", "3",
                 "http://192.168.7.1/api/status"])
            log("DIAG", f"curl 192.168.7.1: {elapsed5:.1f}s {out5.strip()[:200]}")

    # Network interfaces
    out6, _, _ = run_cmd(["ifconfig"], timeout=3)
    for line in out6.split("\n"):
        if "192.168" in line or "169.254" in line or "RNDIS" in line.lower() or "en1" in line:
            log("DIAG", f"ifconfig: {line.strip()}")

    # DNS-SD browse
    log("DIAG", "Browsing _pyro._tcp...")
    p = subprocess.Popen(["dns-sd", "-B", "_pyro._tcp", "local."],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2)
    p.kill()
    browse_out = p.communicate()[0].decode()
    for line in browse_out.strip().split("\n"):
        if line.strip():
            log("DIAG", f"dns-sd browse: {line.strip()}")

    # picotool
    out7, rc7, _ = run_cmd([PICOTOOL, "info", "--vid", "0x2E8A", "--pid", "0x4002"], timeout=5)
    log("DIAG", f"picotool info: rc={rc7} {out7.strip()[:200]}")

    log("DIAG", "=== End diagnostics ===")

# ── Plain text mode ──

def run_plain():
    host = args.host
    plan = define_tests()
    if args.uart:
        start_uart(args.uart)
        if uart_error: print(f"UART error: {uart_error}")
    if args.reset:
        print("Resetting device via picotool...")
        if not picotool_reset(host):
            print("  Reset failed")
            if not args.all: return

    print(f"=== Pyro MK1B Network Test ===")
    print(f"Host: {host}  Mode: {'all' if args.all else 'fail-fast'}\n")

    passed = failed = 0
    printed_sections = set()
    for idx, fn in plan:
        for i in range(idx + 1):
            s, n, st, _ = tests[i]
            if s and not n and s not in printed_sections:
                print(f"\n-- {s} --"); printed_sections.add(s)
        try: ok, detail = fn(host)
        except Exception as e: ok, detail = False, str(e)
        set_result(idx, ok, detail)
        sym = "✓" if ok else "✗"
        extra = f" ({detail})" if detail else ""
        print(f"  {sym} {tests[idx][1]}{extra}")
        if ok: passed += 1
        else:
            failed += 1
            if not args.all: break

    print(f"\n=== Results: {passed} passed, {failed} failed ===")
    stop_uart()
    return failed

# ── Log analyzer ──

def analyze_log(filepath):
    if not os.path.exists(filepath):
        print(f"Error: {filepath} not found"); sys.exit(1)

    lines = open(filepath).readlines()
    sys_info, diag_info, failures, all_tests = [], [], [], []

    for line in lines:
        line = line.rstrip()
        if not line: continue
        try:
            ts = float(line[1:9].strip())
            src = line[12:17].strip()
            msg = line[19:]
        except (ValueError, IndexError):
            continue

        if src == "SYS":
            sys_info.append(msg)
        elif src == "DIAG":
            diag_info.append(msg)
        elif src == "TEST":
            if msg.startswith("PASS"):
                all_tests.append(("PASS", msg[5:], ts))
            elif msg.startswith("FAIL"):
                name = msg[5:]
                all_tests.append(("FAIL", name, ts))
                context = [l.rstrip() for l in lines
                           if l.strip() and abs(float(l[1:9].strip()) - ts) < 2.0]
                failures.append((name, ts, context))

    print("=" * 60)
    print("  Pyro MK1B Test Log Analysis")
    print("=" * 60)
    print("\nSystem Info:")
    for s in sys_info: print(f"  {s}")

    if diag_info:
        print("\nPre-test Diagnostics:")
        for d in diag_info: print(f"  {d}")

    total = len(all_tests)
    p = sum(1 for s, _, _ in all_tests if s == "PASS")
    f = sum(1 for s, _, _ in all_tests if s == "FAIL")
    print(f"\nResults: {p}/{total} passed, {f} failed")

    if not failures:
        print("\n✓ All tests passed — no issues found."); return

    print(f"\n{'='*60}\n  {f} FAILURE(S)\n{'='*60}")
    for name, ts, context in failures:
        print(f"\n✗ {name} (at {ts:.3f}s)")
        print("  Context:")
        for cl in context: print(f"    {cl}")

    print(f"\n{'='*60}\n  Diagnosis\n{'='*60}")
    full = "".join(lines)
    if "out of memory in pool TCP_PCB" in full:
        print("  ⚠ TCP PCB pool exhaustion — increase MEMP_NUM_TCP_PCB")
    if "could not allocate" in full:
        print("  ⚠ Memory allocation failure — increase MEM_SIZE")
    if any("(empty)" in c for _, _, ctx in failures for c in ctx):
        print("  ⚠ Empty responses — possible connection pool leak or TCP stall")
    if any(" 000 " in c for _, _, ctx in failures for c in ctx):
        print("  ⚠ HTTP 000 responses — device not accepting TCP connections")
    if not any("UART" in l for l in lines):
        print("  ℹ No UART data — rerun with --uart for device-side diagnostics")
    if any("Resolving timed out" in l or "dns-sd result" in l and "0b" in l for l in lines):
        print("  ⚠ mDNS resolution failed — try with direct IP (192.168.7.1)")
    if diag_info and any("ping 192.168.7.1: rc=0" in d for d in diag_info):
        if any("(empty)" in c for _, _, ctx in failures for c in ctx):
            print("  ℹ Direct IP reachable but host failed — likely mDNS issue, not device")
    print()

# ── Interactive mode ──

def interactive_setup():
    print("=" * 50)
    print("  Pyro MK1B Network Test — Setup")
    print("=" * 50)
    print()

    host = input("Device address [pyro.local]: ").strip() or "pyro.local"

    r = input("Stop on first failure? [Y/n]: ").strip().lower()
    run_all = r in ("n", "no")

    uart_port = None
    r = input("Monitor UART? [y/N]: ").strip().lower()
    if r in ("y", "yes"):
        ports = sorted(glob.glob("/dev/tty.usbmodem*"))
        if ports:
            print("  Available ports:")
            for i, p in enumerate(ports): print(f"    {i+1}. {p}")
            sel = input(f"  Select port [1]: ").strip()
            idx = int(sel) - 1 if sel.isdigit() else 0
            if 0 <= idx < len(ports): uart_port = ports[idx]
        else:
            uart_port = input("  Enter port path: ").strip() or None

    r = input("Reset device before testing? [y/N]: ").strip().lower()
    do_reset = r in ("y", "yes")

    r = input("Save log file? [Y/n]: ").strip().lower()
    log_path = None
    if r not in ("n", "no"):
        default = f"pyro_test_{time.strftime('%Y%m%d_%H%M%S')}.log"
        log_path = input(f"  Log file [{default}]: ").strip() or default

    # Build repeat command
    cmd = ["python3", "test_network.py"]
    if run_all: cmd.append("--all")
    if uart_port: cmd.extend(["--uart", uart_port])
    if do_reset: cmd.append("--reset")
    if log_path: cmd.extend(["--log", log_path])
    cmd.append(host)
    cmd_line = " ".join(cmd)

    print(f"\nStarting tests...\n")

    args.host = host
    args.all = run_all
    args.uart = uart_port
    args.reset = do_reset
    args.log = log_path
    return cmd_line

# ── Main ──

if __name__ == "__main__":
    if args.analyze:
        analyze_log(args.analyze)
        sys.exit(0)

    cmd_line = None
    has_flags = args.all or args.uart or args.reset or args.log or args.plain
    if args.host is None and not has_flags:
        cmd_line = interactive_setup()
    elif args.host is None:
        args.host = "pyro.local"

    if args.log:
        log_file = open(args.log, "w")
        import platform
        log("SYS", f"Test started host={args.host}")
        log("SYS", f"OS: {platform.system()} {platform.release()} {platform.machine()}")
        log("SYS", f"Python: {sys.version.split()[0]}")
        log("SYS", f"pyserial: {'yes' if HAS_SERIAL else 'no'}")
        log("SYS", f"args: {vars(args)}")
        collect_diagnostics(args.host)

    try:
        if args.plain or not sys.stdout.isatty():
            f = run_plain()
            if cmd_line: print(f"\nTo repeat this test:\n  {cmd_line}")
            sys.exit(1 if f else 0)
        else:
            try: curses.wrapper(draw_tui)
            finally: stop_uart()
            f = sum(1 for _, n, s, _ in tests if n and s == FAILED)
            if cmd_line: print(f"\nTo repeat this test:\n  {cmd_line}")
            sys.exit(1 if f else 0)
    finally:
        if log_file:
            log("SYS", "Test finished")
            log_file.close()
