#!/usr/bin/env python3
"""Comprehensive network test for Pyro MK1B with live TUI.

Usage: ./test_network.py [options] [host]

Options:
  --all           Run all tests even if one fails
  --uart PORT     Monitor UART during tests
  --reset         Reset device via picotool before testing
  --repeat N      Repeat test suite N times
  --plain         Disable TUI, plain text output
  host            Default: pyro.local
"""
import subprocess, sys, time, os, argparse, threading, json, curses

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
parser.add_argument("--plain", action="store_true", help="Plain text output (no TUI)")
args = parser.parse_args()

# ── Test registry ──

PENDING = 0
RUNNING = 1
PASSED = 2
FAILED = 3

tests = []       # [(section, name, state, detail)]
uart_lines = []  # ring buffer of UART lines
uart_error = None
lock = threading.Lock()

def add_section(name):
    with lock:
        tests.append((name, None, PENDING, ""))

def add_test(name):
    with lock:
        tests.append((None, name, PENDING, ""))
    return len(tests) - 1

def set_running(idx):
    with lock:
        s, n, _, d = tests[idx]
        tests[idx] = (s, n, RUNNING, d)

def set_result(idx, ok, detail=""):
    with lock:
        s, n, _, _ = tests[idx]
        tests[idx] = (s, n, PASSED if ok else FAILED, detail)

# ── UART via pyserial ──

uart_ser = None

def start_uart(port):
    global uart_ser, uart_error
    if not HAS_SERIAL:
        uart_error = "pyserial not installed (pip install pyserial)"
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
                            with lock:
                                uart_lines.append(line.decode(errors="replace").rstrip())
                                if len(uart_lines) > 500:
                                    del uart_lines[:100]
                except (serial.SerialException, OSError):
                    break
        t = threading.Thread(target=reader, daemon=True)
        t.start()
    except serial.SerialException as e:
        uart_error = str(e)

def stop_uart():
    global uart_ser
    if uart_ser and uart_ser.is_open:
        uart_ser.close()
    uart_ser = None

# ── picotool ──

def picotool_reset(host):
    subprocess.run([PICOTOOL, "reboot", "-u", "-f", "--vid", "0x2E8A", "--pid", "0x4002"],
                   capture_output=True, timeout=10)
    time.sleep(1)
    subprocess.run([PICOTOOL, "reboot"], capture_output=True, timeout=10)
    for i in range(20):
        time.sleep(1)
        r = subprocess.run(["ping", "-c", "1", "-t", "2", host], capture_output=True)
        if r.returncode == 0:
            time.sleep(1)
            return True
    return False

# ── Test helpers ──

def run_cmd(cmd, timeout=5):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        return "", 1

def curl(host, path, timeout=5):
    out, _ = run_cmd(["curl", "-s", "--max-time", str(timeout), f"http://{host}{path}"])
    return out

def curl_headers(host, path, timeout=5):
    out, _ = run_cmd(["curl", "-s", "-D-", "--max-time", str(timeout), f"http://{host}{path}"])
    return out

# ── Test definitions ──

def define_tests():
    """Register all tests. Returns list of (idx, run_fn) tuples."""
    plan = []

    add_section("Connectivity")
    idx = add_test("ping")
    def t_ping(h):
        _, rc = run_cmd(["ping", "-c", "1", "-t", "3", h])
        return rc == 0, ""
    plan.append((idx, t_ping))

    add_section("HTTP API")
    idx = add_test("GET /api/status returns JSON")
    def t_status(h):
        s = curl(h, "/api/status")
        return "fw_version" in s, f"{len(s)}b"
    plan.append((idx, t_status))

    idx = add_test("status is valid JSON with fields")
    def t_status_fields(h):
        s = curl(h, "/api/status")
        try:
            d = json.loads(s)
            ok = all(k in d for k in ("state", "pressure_pa", "fw_version", "uptime", "pyro1_cont"))
            return ok, d.get("fw_version", "?")
        except:
            return False, "invalid JSON"
    plan.append((idx, t_status_fields))

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
    idx = add_test("GET / (index.html)")
    def t_index(h):
        s = curl(h, "/")
        return len(s) > 900 and "<!DOCTYPE" in s, f"{len(s)}b"
    plan.append((idx, t_index))

    idx = add_test("GET /www/app.js")
    def t_js(h):
        s = curl(h, "/www/app.js")
        return len(s) > 2000 and "function update" in s, f"{len(s)}b"
    plan.append((idx, t_js))

    idx = add_test("GET /www/style.css")
    def t_css(h):
        s = curl(h, "/www/style.css")
        return len(s) > 300, f"{len(s)}b"
    plan.append((idx, t_css))

    add_section("File Consistency")
    idx = add_test("app.js 5x consistent")
    def t_js_consist(h):
        sizes = [len(curl(h, "/www/app.js")) for _ in range(5)]
        ok = len(set(sizes)) == 1 and sizes[0] > 2000
        return ok, str(sizes)
    plan.append((idx, t_js_consist))

    idx = add_test("index.html 5x consistent")
    def t_idx_consist(h):
        sizes = [len(curl(h, "/")) for _ in range(5)]
        ok = len(set(sizes)) == 1 and sizes[0] > 900
        return ok, str(sizes)
    plan.append((idx, t_idx_consist))

    add_section("Error Handling")
    idx = add_test("404 for missing file")
    def t_404(h):
        r = curl(h, "/nonexistent")
        return "Not found" in r or "404" in r, ""
    plan.append((idx, t_404))

    add_section("Parallel Connections")
    idx = add_test("6 parallel status requests")
    def t_par_status(h):
        procs = [subprocess.Popen(["curl", "-s", "--max-time", "5", f"http://{h}/api/status"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(6)]
        results = [p.communicate()[0].decode() for p in procs]
        ok = sum(1 for r in results if "fw_version" in r)
        return ok == 6, f"{ok}/6"
    plan.append((idx, t_par_status))

    idx = add_test("6 parallel mixed requests")
    def t_par_mixed(h):
        paths = ["/", "/www/app.js", "/www/style.css", "/api/status", "/api/config", "/"]
        procs = [subprocess.Popen(["curl", "-s", "--max-time", "5", f"http://{h}{p}"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) for p in paths]
        sizes = [len(p.communicate()[0]) for p in procs]
        return all(s > 0 for s in sizes), str(sizes)
    plan.append((idx, t_par_mixed))

    add_section("Sequential Requests")
    idx = add_test("20 sequential status requests")
    def t_seq_status(h):
        ok = sum(1 for _ in range(20) if "fw_version" in curl(h, "/api/status", 3))
        return ok == 20, f"{ok}/20"
    plan.append((idx, t_seq_status))

    idx = add_test("10 sequential file downloads")
    def t_seq_files(h):
        ok = sum(1 for _ in range(10) if len(curl(h, "/www/app.js")) > 2000)
        return ok == 10, f"{ok}/10"
    plan.append((idx, t_seq_files))

    add_section("mDNS/DNS-SD")
    idx = add_test("_pyro._tcp service found")
    def t_mdns(h):
        p = subprocess.Popen(["dns-sd", "-B", "_pyro._tcp", "local."],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(3)
        p.kill()
        out = p.communicate()[0].decode()
        return "pyro" in out, ""
    plan.append((idx, t_mdns))

    add_section("picotool")
    idx = add_test("picotool sees device")
    def t_picotool(h):
        out, rc = run_cmd([PICOTOOL, "reboot", "-f", "--vid", "0x2E8A", "--pid", "0x4002"], 10)
        return rc == 0 or "RP-series" in out, ""
    plan.append((idx, t_picotool))

    return plan

# ── TUI rendering ──

def draw_tui(stdscr):
    curses.curs_set(0)
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_GREEN, -1)   # passed
    curses.init_pair(2, curses.COLOR_RED, -1)      # failed
    curses.init_pair(3, curses.COLOR_WHITE, -1)    # running/bold
    curses.init_pair(4, 8, -1)                     # grey (pending) - color 8 = bright black

    host = args.host
    plan = define_tests()

    # Start UART
    if args.uart:
        start_uart(args.uart)

    # Reset
    if args.reset:
        with lock:
            tests.insert(0, ("Setup", None, PENDING, ""))
            tests.insert(1, (None, "picotool reset", RUNNING, ""))
        refresh_screen(stdscr, host)
        ok = picotool_reset(host)
        with lock:
            tests[1] = (None, "picotool reset", PASSED if ok else FAILED, "")
        refresh_screen(stdscr, host)
        if not ok and not args.all:
            time.sleep(2)
            return

    # Run tests
    for idx, fn in plan:
        set_running(idx)
        refresh_screen(stdscr, host)
        try:
            ok, detail = fn(host)
        except Exception as e:
            ok, detail = False, str(e)
        set_result(idx, ok, detail)
        refresh_screen(stdscr, host)
        if not ok and not args.all:
            break

    # Final display
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

    # Header
    p = sum(1 for _, n, s, _ in tests if n and s == PASSED)
    f = sum(1 for _, n, s, _ in tests if n and s == FAILED)
    t = sum(1 for _, n, _, _ in tests if n)
    header = f" Pyro MK1B Test — {host} — {p}/{t} passed"
    if f:
        header += f", {f} failed"
    safe_addstr(stdscr, 0, 0, header[:w-1], curses.A_BOLD)

    # Test list
    row = 2
    for sec, name, state, detail in tests:
        if row >= h - 2:
            break
        if sec and not name:
            safe_addstr(stdscr, row, 1, f"── {sec} ──", curses.A_BOLD)
            row += 1
            continue

        if state == PASSED:
            sym, attr = "✓", curses.color_pair(1)
        elif state == FAILED:
            sym, attr = "✗", curses.color_pair(2)
        elif state == RUNNING:
            sym, attr = "▸", curses.color_pair(3) | curses.A_BOLD
        else:
            sym, attr = "·", curses.color_pair(4)

        line = f" {sym} {name}"
        if detail and state in (PASSED, FAILED):
            line += f"  ({detail})"
        safe_addstr(stdscr, row, 1, line[:left_w-2], attr)
        row += 1

    # UART panel
    if has_uart:
        # Divider
        for r in range(h):
            safe_addstr(stdscr, r, left_w, "│")

        uart_header = f" UART"
        if uart_error:
            uart_header += f" ⚠ {uart_error}"
        safe_addstr(stdscr, 0, right_x, uart_header[:w-right_x-1], curses.A_BOLD)

        with lock:
            visible = uart_lines[-(h-3):]
        for i, line in enumerate(visible):
            r = 2 + i
            if r >= h - 1:
                break
            safe_addstr(stdscr, r, right_x + 1, line[:w-right_x-2])

    stdscr.refresh()

def safe_addstr(stdscr, y, x, s, attr=0):
    h, w = stdscr.getmaxyx()
    if y < h and x < w:
        try:
            stdscr.addnstr(y, x, s, w - x - 1, attr)
        except curses.error:
            pass

# ── Plain text mode ──

def run_plain():
    host = args.host
    plan = define_tests()

    if args.uart:
        start_uart(args.uart)
        if uart_error:
            print(f"UART error: {uart_error}")

    if args.reset:
        print("Resetting device via picotool...")
        if not picotool_reset(host):
            print("  Reset failed")
            if not args.all:
                return

    print(f"=== Pyro MK1B Network Test ===")
    print(f"Host: {host}  Mode: {'all' if args.all else 'fail-fast'}\n")

    passed = failed = 0
    for idx, fn in plan:
        sec, name, _, _ = tests[idx]
        # Print section headers
        for i in range(idx):
            s, n, st, _ = tests[i]
            if s and not n and st == PENDING:
                print(f"\n-- {s} --")
                tests[i] = (s, n, RUNNING, "")
        try:
            ok, detail = fn(host)
        except Exception as e:
            ok, detail = False, str(e)
        sym = "✓" if ok else "✗"
        extra = f" ({detail})" if detail else ""
        print(f"  {sym} {name}{extra}")
        set_result(idx, ok, detail)
        if ok:
            passed += 1
        else:
            failed += 1
            if not args.all:
                break

    if uart_lines:
        print(f"\n=== UART LOG ({len(uart_lines)} lines) ===")
        for line in uart_lines[-30:]:
            print(f"  {line}")

    print(f"\n=== Results: {passed} passed, {failed} failed ===")
    stop_uart()
    sys.exit(1 if failed else 0)

# ── Main ──

if __name__ == "__main__":
    if args.plain or not sys.stdout.isatty():
        run_plain()
    else:
        try:
            curses.wrapper(draw_tui)
        finally:
            stop_uart()
        f = sum(1 for _, n, s, _ in tests if n and s == FAILED)
        sys.exit(1 if f else 0)
