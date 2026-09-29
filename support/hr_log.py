#!/usr/bin/env python3
"""Decode a high-rate log (src/sd/hr_log.h) into CSV files and a summary.

    support/hr_log.py logs/hr0001.bin             writes hr0001_imu.csv,
                                                  hr0001_pres.csv, hr0001_flight.csv
    support/hr_log.py --summary logs/hr0001.bin   the summary only
    support/hr_log.py --fetch 192.168.42.1 hr0001 fetch it from a board first
    support/hr_log.py --selftest

A record is: u8 type, u8 flags, u16 payload length, u16 CRC-16/XMODEM of the
payload, then the payload, little-endian. The log ends at the first record
whose CRC does not match: a power cut leaves the last one's payload
unwritten, and the preallocated space past it holds what the card held.

IMU times: the newest set of a batch was backlog_words/6 sets from the end of
the FIFO when read_us was stamped, and the sets before it are one ODR period
apart. The timer is 32 bits of microseconds; times are unwrapped from the
header's open_us.
"""
import argparse
import os
import struct
import sys
import urllib.request

REC = struct.Struct("<BBHH")
HEADER = struct.Struct("<4sHHIIIII16s16s")
IMU = struct.Struct("<IHBB")
SET = struct.Struct("<6h")
PRES = struct.Struct("<IIIIiB3x")
FLIGHT = struct.Struct("<IBBHiii")
T_HEADER, T_IMU, T_PRES, T_FLIGHT = 1, 2, 3, 4

STATES = ["BOOT_SETTLE", "BOOT_CONTINUITY", "BOOT_CALIBRATE", "PAD_IDLE", "ASCENT", "FALLING",
          "DROGUE_DESCENT", "CHUTE_DESCENT", "LANDED", "BOOT_SENSOR", "FAULT", "GROUND_TEST"]


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Unwrap:
    """32-bit microseconds to a monotonic count, from a first value."""

    def __init__(self, first):
        self.last = first
        self.base = 0

    def __call__(self, t):
        if t < self.last and self.last - t > 0x80000000:
            self.base += 1 << 32
        elif t > self.last and t - self.last > 0x80000000:
            return self.base - (1 << 32) + t  # a sample stamped before the last seen
        self.last = t
        return self.base + t


def records(blob):
    o = 0
    while o + REC.size <= len(blob):
        typ, _flags, n, crc = REC.unpack_from(blob, o)
        body = blob[o + REC.size:o + REC.size + n]
        if typ not in (T_HEADER, T_IMU, T_PRES, T_FLIGHT) or len(body) != n or crc16(body) != crc:
            return
        yield typ, body
        o += REC.size + n


def decode(blob):
    out = {"header": None, "imu": [], "pres": [], "flight": [], "overruns": 0, "batches": 0, "bytes": 0}
    unwrap = None
    odr = 1
    for typ, body in records(blob):
        out["bytes"] += REC.size + len(body)
        if typ == T_HEADER:
            (magic, ver, _hl, odr, ug, mdps, open_us, open_ms, board, reason) = HEADER.unpack_from(body)
            if magic != b"PYHR":
                raise ValueError("not a high-rate log")
            out["header"] = {"version": ver, "odr_hz": odr, "ug_per_lsb": ug, "mdps_per_lsb": mdps,
                             "open_us": open_us, "open_ms": open_ms,
                             "board": board.split(b"\0")[0].decode(), "reason": reason.split(b"\0")[0].decode()}
            unwrap = Unwrap(open_us)
        elif out["header"] is None:
            raise ValueError("the first record is not the header")
        elif typ == T_IMU:
            read_us, backlog, overrun, _ = IMU.unpack_from(body)
            n = (len(body) - IMU.size) // SET.size
            h = out["header"]
            t_read = unwrap(read_us) - h["open_us"]
            newest = t_read - (backlog // 6) * 1e6 / odr
            out["batches"] += 1
            out["overruns"] += overrun
            for i in range(n):
                gx, gy, gz, ax, ay, az = SET.unpack_from(body, IMU.size + i * SET.size)
                t = newest - (n - 1 - i) * 1e6 / odr
                out["imu"].append((t / 1e6,
                                   ax * h["ug_per_lsb"] / 1e6, ay * h["ug_per_lsb"] / 1e6, az * h["ug_per_lsb"] / 1e6,
                                   gx * h["mdps_per_lsb"] / 1e3, gy * h["mdps_per_lsb"] / 1e3,
                                   gz * h["mdps_per_lsb"] / 1e3))
        elif typ == T_PRES:
            at_us, read_us, raw, raw_t, pa_c, kind = PRES.unpack_from(body)
            out["pres"].append(((unwrap(at_us) - out["header"]["open_us"]) / 1e6, chr(kind), raw, raw_t,
                                pa_c / 100.0))
        elif typ == T_FLIGHT:
            t_us, state, thrust, _, alt, speed, pa = FLIGHT.unpack_from(body)
            out["flight"].append(((unwrap(t_us) - out["header"]["open_us"]) / 1e6,
                                  STATES[state] if state < len(STATES) else str(state), thrust, alt / 100.0,
                                  speed / 100.0, pa))
    return out


def summary(d):
    h = d["header"]
    imu = d["imu"]
    lines = [f"{h['board']}, {h['reason']}, v{h['version']}: IMU at {h['odr_hz']} Hz, "
             f"{d['bytes']} bytes of records"]
    if imu:
        span = imu[-1][0] - imu[0][0]
        rate = (len(imu) - 1) / span if span > 0 else 0
        peak = max(abs(s[i]) for s in imu for i in (1, 2, 3))
        lines.append(f"IMU: {len(imu)} sets in {d['batches']} batches over {span:.3f} s, {rate:.1f} sets/s, "
                     f"{d['overruns']} FIFO overruns, peak |axis| {peak:.2f} g")
    if d["pres"]:
        p = [r for r in d["pres"] if r[1] == "P"]
        lines.append(f"pressure: {len(p)} conversions, {len(d['pres']) - len(p)} other trace records")
    if d["flight"]:
        states = []
        for f in d["flight"]:
            if not states or states[-1] != f[1]:
                states.append(f[1])
        lines.append(f"flight: {len(d['flight'])} snapshots; states {' -> '.join(states)}; "
                     f"max {max(f[3] for f in d['flight']):.1f} m")
    return "\n".join(lines)


def write_csv(d, base):
    with open(base + "_imu.csv", "w") as f:
        f.write("t_s,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n")
        for s in d["imu"]:
            f.write("%.6f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n" % s)
    with open(base + "_pres.csv", "w") as f:
        f.write("t_s,kind,raw,raw_t,pa\n")
        for r in d["pres"]:
            f.write("%.6f,%s,%d,%d,%.2f\n" % r)
    with open(base + "_flight.csv", "w") as f:
        f.write("t_s,state,thrust,alt_m,speed_ms,pressure_pa\n")
        for r in d["flight"]:
            f.write("%.3f,%s,%d,%.2f,%.2f,%d\n" % r)


def encode(recs):
    """The firmware's encoding, for the self-test."""
    out = b""
    for typ, body in recs:
        out += REC.pack(typ, 0, len(body), crc16(body)) + body
    return out


def selftest():
    hdr = HEADER.pack(b"PYHR", 2, HEADER.size, 1660, 488, 70, 0xFFFFF000, 5000, b"Pyro MK1C-SD", b"flight")
    hdr = hdr + bytes(4096 - REC.size - len(hdr))
    recs = [(T_HEADER, hdr)]
    seq = 0
    t = 0xFFFFF000
    for batch in range(100):
        t = (t + 10240) & 0xFFFFFFFF
        sets = b"".join(SET.pack(seq + i, 0, 0, 0, 0, 2049) for i in range(17))
        seq += 17
        recs.append((T_IMU, IMU.pack(t, 0, 0, 0) + sets))
        recs.append((T_PRES, PRES.pack(t - 3000 & 0xFFFFFFFF, t, 4000000, 0, 10132500, ord("P"))))
        if batch % 10 == 0:
            recs.append((T_FLIGHT, FLIGHT.pack(t, 4, 1, 0, 12345, 6789, 100000)))
    blob = encode(recs) + bytes(512)  # the zeros past a power cut
    torn = bytearray(encode(recs[:5]))
    torn[-3] ^= 0xFF  # a last record whose payload never reached the card
    d = decode(blob)
    ok = True
    ok &= d["header"]["odr_hz"] == 1660 and d["header"]["reason"] == "flight"
    ok &= len(d["imu"]) == 1700 and d["batches"] == 100
    ok &= [int(round(s[1] * 1e6 / 488)) for s in d["imu"][:3]] == [0, 0, 0]
    ok &= abs(d["imu"][0][3] - 1.0) < 0.001  # 2049 * 488 ug
    ts = [s[0] for s in d["imu"]]
    ok &= all(b >= a for a, b in zip(ts, ts[1:]))  # through the 32-bit wrap
    ok &= len(d["pres"]) == 100 and abs(d["pres"][0][4] - 101325.0) < 0.01
    ok &= len(d["flight"]) == 10 and d["flight"][0][1] == "ASCENT"
    dt = decode(bytes(torn))
    ok &= dt["batches"] == 1  # stops at the torn record, keeps the whole ones
    print("hr_log selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--fetch", metavar="HOST")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.file:
        ap.error("a file (or --selftest)")
    path = a.file
    if a.fetch:
        name = os.path.basename(path)
        if not name.endswith(".bin"):
            name += ".bin"
        with urllib.request.urlopen(f"http://{a.fetch}/logs/{name}", timeout=120) as r:
            data = r.read()
        with open(name, "wb") as f:
            f.write(data)
        path = name
    with open(path, "rb") as f:
        d = decode(f.read())
    print(summary(d))
    if not a.summary:
        base = os.path.splitext(path)[0]
        write_csv(d, base)
        print(f"wrote {base}_imu.csv, {base}_pres.csv, {base}_flight.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
