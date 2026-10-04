#!/usr/bin/env python3
"""Network throughput of a board built with -DPYRO_NET_BENCH=ON, whose
routes move bytes with no storage behind them: GET /api/net/blob?n=<bytes>
and POST /api/net/sink. For comparing TCP/IP stacks and hosts
(docs/smallest_tcp_evaluation.md).

Usage:
    ./support/net_bench.py 192.168.42.1 [down] [up] [par] [ping]

SPDX-License-Identifier: MIT
"""
import concurrent.futures
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

if len(sys.argv) < 2:
    sys.exit(__doc__)
HOST = sys.argv[1]
what = sys.argv[2:] or ["down", "up", "par", "ping"]


def req(method, path, data=None, timeout=300):
    r = urllib.request.Request(f"http://{HOST}{path}", data=data, method=method, headers={"X-Pyro": "1"})
    t0 = time.time()
    try:
        with urllib.request.urlopen(r, timeout=timeout) as f:
            body = f.read()
            return f.status, body, time.time() - t0
    except urllib.error.HTTPError as e:
        return e.code, e.read(), time.time() - t0
    except OSError as e:
        return 0, repr(e).encode(), time.time() - t0


def rate(n, t):
    return f"{n / t / 1000:.1f} kB/s"


if "down" in what:
    for size, runs in ((74048, 5), (1000000, 3)):
        for _ in range(runs):
            code, body, t = req("GET", f"/api/net/blob?n={size}")
            print(f"GET {size} B: {code} whole={len(body) == size} {t:.3f}s {rate(len(body), t)}")

if "up" in what:
    blob = os.urandom(300_000)
    for _ in range(3):
        code, body, t = req("POST", "/api/net/sink", blob)
        print(f"POST {len(blob)} B: {code} {body[:40]!r} {t:.2f}s {rate(len(blob), t)}")

if "par" in what:
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        res = list(ex.map(lambda _: req("GET", "/api/net/blob?n=74048", timeout=120), range(8)))
    t = time.time() - t0
    whole = sum(1 for c, b, _ in res if c == 200 and len(b) == 74048)
    total = sum(len(b) for c, b, _ in res if c == 200)
    print(f"8 at once, 74048 B each: {whole}/8 whole, {t:.2f}s {rate(max(total, 1), t)}")

if "ping" in what:
    p = subprocess.run(["ping", "-c", "20", "-i", "0.2", HOST], capture_output=True, text=True)
    print("ping:", " | ".join(p.stdout.strip().splitlines()[-2:]))

code, body, _ = req("GET", "/api/net")
try:
    print("net:", json.loads(body))
except ValueError:
    print("net:", code, body[:200])
