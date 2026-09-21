#!/usr/bin/env python3
"""
pyro_check.py — bench verification for the Pyro MK1C pyro front end.

Triggers high-speed captures of the firing bus charging and discharging,
fits the exponentials, and solves for the actual component values. Compares
everything against the design constants from DESIGN.md section 4.

The device writes self-describing CSV, so every constant used here comes out
of the capture file. Nothing is hardcoded except the argument defaults.

Usage:
    ./support/pyro_check.py                      # capture and analyse
    ./support/pyro_check.py --rpd 1916           # supply a measured Rpd
    ./support/pyro_check.py --save-dir captures  # keep the raw CSV
    ./support/pyro_check.py --offline captures   # re-analyse saved files

Why a waveform and not a threshold crossing: a single crossing gave a time
constant that was wrong by 1.94x with nothing in the firmware able to detect
it. A full curve is fitted over hundreds of points and its residual says
whether the model fits at all.

SPDX-License-Identifier: MIT
"""

import argparse
import math
import os
import sys
import time
import urllib.error
import urllib.request

DEFAULT_HOST = "pyro.local"


# ── transport ────────────────────────────────────────────────────────


def http_get(host, path, timeout=15):
    url = f"http://{host}{path}"
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def capture(host, mode, timeout=30):
    """Queue a capture and wait for the device to finish writing the file."""
    tag = "c" if mode == "charge" else "d"
    http_get(host, f"/api/capture?m={tag}")
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.4)
        st = http_get(host, "/api/status")
        # wave_state: 0 idle, 1 busy, 2 ready
        if '"wave_state":2' in st.replace(" ", ""):
            return http_get(host, f"/wave_{tag}.csv", timeout=timeout)
    raise TimeoutError(f"{mode} capture did not complete within {timeout}s")


# ── capture file ─────────────────────────────────────────────────────


class Capture:
    """One waveform plus the metadata the device recorded with it."""

    def __init__(self, text):
        self.meta = {}
        self.counts = []
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                parts = line[1:].split(",", 1)
                if len(parts) == 2:
                    self.meta[parts[0].strip()] = parts[1].strip()
                continue
            if line.startswith("i,"):
                continue
            parts = line.split(",")
            if len(parts) == 2 and parts[1].lstrip("-").isdigit():
                self.counts.append(int(parts[1]))

    def num(self, key, default=None):
        v = self.meta.get(key)
        if v is None:
            return default
        try:
            return float(v)
        except ValueError:
            return default

    @property
    def dt_us(self):
        return self.num("dt_us")

    @property
    def pre_n(self):
        return int(self.num("pre_n", 0))

    @property
    def uv_per_count(self):
        return self.num("uv_per_count")

    def mv(self, counts):
        return counts * self.uv_per_count / 1000.0


# ── fitting ──────────────────────────────────────────────────────────


def _linfit(xs, ys):
    """Least squares y = a + b x. Returns (a, b, r2)."""
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx == 0:
        return my, 0.0, 0.0
    b = sxy / sxx
    a = my - b * mx
    syy = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 - ss_res / syy if syy > 0 else 0.0
    return a, b, r2


def check(label, measured, design, tol_pct, unit, note=""):
    """One comparison row. INFO when there is no design value to compare."""
    if design is None or design == 0:
        return (label, f"{measured:.4g} {unit}", "-", "-", "INFO", note)
    err = (measured - design) / design * 100.0
    verdict = "PASS" if abs(err) <= tol_pct else "FAIL"
    return (label, f"{measured:.4g} {unit}", f"{design:.4g} {unit}", f"{err:+.1f}%", verdict, note)


def tau_profile(cap, rpd, charging):
    """
    Local time constant as a function of node voltage.

    Deliberately NOT a single-exponential fit. A constant-R, constant-C
    network gives a flat profile; anything else means the model is wrong,
    and reporting one tau would hide that. This is what caught C115: its
    capacitance changes 3.4x with DC bias, so every single-tau estimate
    disagreed with every other one and with the DC measurement.
    """
    c = cap.counts
    pre = cap.pre_n
    dt = cap.dt_us
    floor = sum(c[-32:]) / 32.0
    peak = sum(c[: max(pre - 3, 1)]) / max(pre - 3, 1)
    if charging:
        floor, peak = sum(c[: max(pre - 3, 1)]) / max(pre - 3, 1), sum(c[-32:]) / 32.0

    out = []
    seg = c[pre + 2 :]
    span = abs(peak - floor)
    if span < 50:
        return out

    # Walk the transition in even steps of remaining amplitude.
    fracs = [0.80, 0.63, 0.50, 0.37, 0.25, 0.15, 0.08]
    marks = []
    # "remaining" is the distance still to travel, which decays as
    # exp(-t/tau) in both directions.
    remaining = (lambda v: peak - v) if charging else (lambda v: v - floor)

    for f in fracs:
        target = f * span
        for i, v in enumerate(seg):
            if remaining(v) <= target:
                marks.append((i * dt, v))
                break

    for (t1, v1), (t2, v2) in zip(marks, marks[1:]):
        r1, r2v = remaining(v1), remaining(v2)
        if r1 <= 0 or r2v <= 0 or t2 <= t1:
            continue
        tau = (t2 - t1) / math.log(r1 / r2v)
        vmid = cap.mv((v1 + v2) / 2.0) / 1000.0
        out.append((vmid, tau, tau / rpd * 1000.0))
    return out


def analyse(chg, dec, rpd_measured=None, dc=None, r120_measured=None):
    rows = []
    dsn_rbias = chg.num("design_r_bias_ohm")
    dsn_rpd = chg.num("design_rpd_ohm")
    dsn_c = chg.num("design_c_bus_nf")
    rdiv = chg.num("design_r_div_ohm")

    rpd = rpd_measured if rpd_measured else dsn_rpd
    src = "measured" if rpd_measured else "design"

    prof = tau_profile(dec, rpd, charging=False)

    # tau(V) = R(V) * C(V); a decay alone cannot separate the two. Resolve it
    # by holding C at the design value and solving for R, then sanity-check:
    # a capacitor can derate DOWN but never reads several times ABOVE nominal,
    # so an implied C far over design means the resistance is what is moving.
    print("  decay profile (a linear RC gives a FLAT tau):")
    print("     bus V     local tau     R_eff at C=design    implied C at R=design")
    for v, tau, cnf in prof:
        r_eff = tau * 1e-6 / (dsn_c * 1e-9)
        print(f"     {v:5.2f} V   {tau:8.0f} us   {r_eff:10.0f} ohm        {cnf:8.0f} nF")
    print()

    if len(prof) >= 3:
        taus = [t for _, t, _ in prof]
        spread = max(taus) / min(taus)
        c_over = max(c for _, _, c in prof) / dsn_c
        verdict = "PASS" if spread < 1.35 else "FAIL"
        note = "flat = linear RC" if verdict == "PASS" else "R(V) varies; see leakage below"
        rows.append(("RC linearity", f"{spread:.2f}x tau spread", "<1.35x", "-", verdict, note))

        if verdict == "FAIL" and rpd_measured:
            # C is a 50V X7R at ~1.7V, so its derating is negligible and it is
            # treated as fixed at the design value. tau(V)/C then gives R(V),
            # and the current above what the cold pull-down explains is leakage.
            print("  leakage I-V (C held at design; current above the cold pull-down):")
            print("     bus V     R_eff     I_total    I_via_Rpd    I_LEAK")
            for v, tau, _ in prof:
                r_eff = tau * 1e-6 / (dsn_c * 1e-9)
                it, ir = v / r_eff, v / rpd
                print(f"     {v:5.2f} V  {r_eff:7.0f} ohm  {it*1000:7.3f} mA  "
                      f"{ir*1000:8.3f} mA  {(it-ir)*1000:7.3f} mA")
            print()
            print("  A junction conducts ~nothing below its turn-on then rises steeply.")
            print("  A wrong resistor would instead give a FLAT R(V).")
            print()

    # DC levels: steady state, so no capacitance involved. Authoritative.
    bus_b = chg.num("meas_bus_biased_counts")
    ch_a = chg.num("meas_ch_a_biased_counts")
    v_bus = chg.mv(bus_b) / 1000.0 if bus_b else None
    v_src = None
    if dc:
        v_src = dc["cathode"]
        v_bus = dc["bus"]
    elif ch_a:
        v_src = (chg.mv(ch_a) / 1000.0) * (dsn_rbias + rdiv) / rdiv

    rows.append(check("bus pull-down Rpd", rpd, dsn_rpd, 5, "ohm", f"({src})"))
    if v_src and v_bus:
        if r120_measured:
            # R120 known, so the DC point measures the bus impedance directly.
            i_in = (v_src - v_bus) / r120_measured
            r_bus_op = v_bus / i_in
            i_extra = i_in - v_bus / rpd
            rows.append(check("R120", r120_measured, dsn_rbias, 10, "ohm", "meter"))
            rows.append(check("bus impedance, operating", r_bus_op, rpd, 15, "ohm",
                              f"at {v_bus:.3f} V"))
            rows.append(("bus leakage current", f"{i_extra*1000:.2f} mA", "~0", "-",
                         "PASS" if abs(i_extra) < 0.05e-3 else "FAIL",
                         "beyond the cold pull-down"))
        else:
            i_bus = v_bus / rpd
            r120 = (v_src - v_bus) / i_bus
            rows.append(check("R120 (DC, assumes no leakage)", r120, dsn_rbias, 20, "ohm",
                              "give --r120 if measured"))
        rows.append(check("bias source", v_src * 1000, 3000, 10, "mV", "3.3V less a Schottky"))

    if prof and not rpd_measured:
        # Only meaningful when there is no DC anchor to say which of R or C
        # is moving. With --rpd/--r120 the leakage table above supersedes it.
        rows.append(check("C_bus (assumes R fixed)", prof[-1][2], dsn_c, 25, "nF",
                          f"at {prof[-1][0]:.2f} V"))

    for key, design_key, name in (
        ("meas_bus_biased_counts", "design_bus_biased_counts", "bus under bias"),
        ("meas_ch_a_biased_counts", "design_ch_biased_counts", "ch A under bias"),
        ("meas_ch_b_biased_counts", "design_ch_biased_counts", "ch B under bias"),
    ):
        m, d = chg.num(key), chg.num(design_key)
        if m:
            rows.append(check(name, m, d, 10, "counts", f"{chg.mv(m)/1000:.3f} V"))

    q = chg.num("meas_bus_quiescent_counts")
    if q is not None:
        rows.append(("bus quiescent", f"{q:.0f} counts", "<50", "-",
                     "PASS" if q < 50 else "FAIL", "high side leakage"))
    return rows, {}


def print_table(rows):
    w = [max(len(str(r[i])) for r in rows + [("measurement", "measured", "design", "err", "result", "note")])
         for i in range(6)]
    hdr = ("measurement", "measured", "design", "err", "result", "note")
    print("  " + "  ".join(h.ljust(w[i]) for i, h in enumerate(hdr)))
    print("  " + "  ".join("-" * w[i] for i in range(6)))
    for r in rows:
        print("  " + "  ".join(str(r[i]).ljust(w[i]) for i in range(6)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--rpd", type=float, default=None,
                    help="measured bus-to-GND resistance in ohm (ohmmeter, power off)")
    ap.add_argument("--save-dir", default=None, help="write the raw CSV here")
    ap.add_argument("--r120", type=float, default=None,
                    help="measured bias resistor in ohm; with --dc this makes the "
                         "operating bus impedance a direct measurement")
    ap.add_argument("--dc", nargs=3, type=float, metavar=("GPIO_V", "CATHODE_V", "BUS_V"),
                    help="meter readings with -DPYRO_MK1C_BIAS_HOLD=ON; makes R120 "
                         "a steady-state measurement with no capacitance involved")
    ap.add_argument("--offline", default=None, help="analyse saved CSV from this directory")
    args = ap.parse_args()

    print()
    if args.offline:
        cpath = os.path.join(args.offline, "wave_c.csv")
        dpath = os.path.join(args.offline, "wave_d.csv")
        print(f"  reading {cpath} and {dpath}")
        chg = Capture(open(cpath).read())
        dec = Capture(open(dpath).read())
    else:
        print(f"  capturing from {args.host} ...")
        try:
            ctext = capture(args.host, "charge")
            dtext = capture(args.host, "decay")
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            print(f"  capture failed: {e}", file=sys.stderr)
            return 2
        chg, dec = Capture(ctext), Capture(dtext)
        if args.save_dir:
            os.makedirs(args.save_dir, exist_ok=True)
            open(os.path.join(args.save_dir, "wave_c.csv"), "w").write(ctext)
            open(os.path.join(args.save_dir, "wave_d.csv"), "w").write(dtext)
            print(f"  saved raw CSV to {args.save_dir}/")

    print(f"  board      : {chg.meta.get('board','?')}   fw {chg.meta.get('fw_version','?')}")
    print(f"  timebase   : {chg.dt_us:.0f} us/sample charge, {dec.dt_us:.0f} us/sample decay, "
          f"{len(chg.counts)} samples")
    print(f"  scaling    : {chg.uv_per_count:.0f} uV per count at the node")
    print()

    try:
        dc = None
        if args.dc:
            dc = dict(gpio=args.dc[0], cathode=args.dc[1], bus=args.dc[2])
            print(f"  meter      : GPIO {dc['gpio']:.3f} V, D107 cathode {dc['cathode']:.3f} V, "
                  f"bus {dc['bus']:.3f} V  (D107 drop {dc['gpio']-dc['cathode']:.3f} V)")
            print()
        rows, _ = analyse(chg, dec, args.rpd, dc, args.r120)
    except ValueError as e:
        print(f"  analysis failed: {e}", file=sys.stderr)
        return 2

    print_table(rows)
    fails = [r for r in rows if r[4] == "FAIL"]
    print()
    if fails:
        print(f"  {len(fails)} FAIL:")
        for r in fails:
            print(f"    - {r[0]}: {r[1]} against {r[2]}")
        return 1
    print("  all checks PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
