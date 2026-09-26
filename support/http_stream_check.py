#!/usr/bin/env python3
"""
http_stream_check.py -- the HTTP server against a byte stream, on a live board.

The server must not depend on how TCP happens to cut a request. This sends
requests over raw sockets in deliberately awkward pieces -- the header block a
byte at a time, the body in a later write, two requests in one write -- and
checks every answer is complete and framed by Content-Length. It also
round-trips web files through an upload in odd chunk sizes, fetches in
parallel, and checks that a stalled connection does not stop the others.

REWRITES /www/app.js and /www/index.html with the local copies (the same
bytes the board should already hold) and turns test mode on and off. Run on
a board in PAD_IDLE.

Usage:
    ./support/http_stream_check.py 192.168.7.1

SPDX-License-Identifier: MIT
"""
import concurrent.futures
import json
import os
import random
import socket
import sys
import time

HOST = sys.argv[1]
WWW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "www")
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail else ""), flush=True)


def exchange(pieces, delay=0.0, timeout=20):
    """Send each piece as its own write, pausing between them, and read the
    whole response. Returns (status, headers, body) with the body framed by
    Content-Length, or raises."""
    s = socket.create_connection((HOST, 80), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    data = b""
    try:
        for p in pieces:
            s.sendall(p)
            if delay:
                time.sleep(delay)
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except OSError:
        pass  # the server hung up early: judged by what came back
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    while head.startswith(b"HTTP/1.1 100 "):
        head, _, body = body.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    try:
        status = int(lines[0].split()[1])
    except (IndexError, ValueError):
        return 0, {}, data
    hdrs = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    return status, hdrs, body


def framed(status, hdrs, body):
    return "content-length" in hdrs and int(hdrs["content-length"]) == len(body)


def request(method, path, body=b"", extra=""):
    h = f"{method} {path} HTTP/1.1\r\nHost: {HOST}\r\n{extra}"
    if body or method == "POST":
        h += f"Content-Length: {len(body)}\r\n"
    return h.encode() + b"\r\n" + body


def status_json():
    st, h, b = exchange([request("GET", "/api/status")])
    return json.loads(b)


st0 = status_json()
if st0["state"] != "PAD_IDLE":
    sys.exit(f"{HOST} is in {st0['state']}; run this on the pad only")
print(f"== {st0['board']} {st0['fw_version']} at {HOST}")

# ── How the request is cut ──────────────────────────────────────────
req = request("GET", "/api/status")
st, h, b = exchange([req[i:i + 1] for i in range(len(req))], delay=0.002)
check("header block one byte per write", st == 200 and framed(st, h, b) and b'"board"' in b, f"{st}")

play = request("POST", "/api/beeps/play", b'{"kind":"chirp","d1":0,"d2":0}')
cut = play.index(b"\r\n\r\n") + 4
st, h, b = exchange([play[:cut], play[cut:]], delay=0.3)
check("body in a later write (the audition, N22)", st in (200, 409) and framed(st, h, b), f"{st} {b[:60]!r}")

on = request("POST", "/api/test_mode/on")
st, h, b = exchange([on[:7], on[7:20], on[20:]], delay=0.05)
check("request line cut mid-word", st == 200 and b'"test_mode":true' in b, f"{st} {b!r}")
st, h, b = exchange([request("POST", "/api/test_mode/off")])
check("test mode off", st == 200 and b'"test_mode":false' in b, f"{st} {b!r}")

st, h, b = exchange([request("GET", "/api/status") + request("GET", "/api/status")])
check("two requests in one write: one answer", st == 200 and framed(st, h, b) and b.count(b'"board"') == 1)

big_head = request("GET", "/api/status", extra="".join(f"X-Filler-{i}: {'y' * 60}\r\n" for i in range(40)))
st, h, b = exchange([big_head[:900], big_head[900:]], delay=0.1)
check(f"{len(big_head)}-byte header block", st == 200 and framed(st, h, b))

st, h, b = exchange([b"PUT /api/status HTTP/1.1\r\n\r\n"])
check("unsupported method -> 405 with Allow", st == 405 and "allow" in h, f"{st} {h.get('allow')}")
st, h, b = exchange([b"POST /api/config HTTP/1.1\r\nContent-Length: 99999\r\n\r\n"])
check("oversized body -> 413 before it is sent", st == 413 and framed(st, h, b), f"{st}")
st, h, b = exchange([b"HEAD /api/status HTTP/1.1\r\n\r\n"])
check("HEAD: headers, no body", st == 200 and b == b"" and int(h.get("content-length", 0)) > 500)

# ── Uploads round-trip, in odd pieces ───────────────────────────────
random.seed(7)
for name in ("app.js", "index.html"):
    data = open(os.path.join(WWW, name), "rb").read()
    upload = request("POST", f"/www/{name}", data)
    pieces, i = [], 0
    while i < len(upload):
        n = random.choice((1, 7, 536, 1460, 3000, 5000))
        pieces.append(upload[i:i + n])
        i += n
    t0 = time.time()
    st, h, b = exchange(pieces)
    check(f"upload /www/{name} ({len(data)} B in {len(pieces)} writes)", st == 201, f"{st} {b!r} {time.time() - t0:.1f}s")
    st, h, back = exchange([request("GET", f"/www/{name}")])
    check(f"GET /www/{name} returns exactly what was sent", st == 200 and back == data and framed(st, h, back),
          f"{len(back)} B")

# ── In parallel, and past a stalled connection ──────────────────────
stall = socket.create_connection((HOST, 80), timeout=30)
stall.sendall(b"GET /api/status HTTP/1.1\r\nHost: x\r\nX-Half: ")  # and nothing more

def fetch(path):
    st, h, b = exchange([request("GET", path)])
    return path, st, framed(st, h, b), len(b)

paths = ["/api/status", "/www/app.js", "/api/beeps", "/api/pins/caps", "/www/index.html", "/api/config"] * 2
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
    got = list(ex.map(fetch, paths))
bad = [g for g in got if g[1] != 200 or not g[2]]
check(f"{len(paths)} fetches, 8 at a time, past a stalled connection", not bad, str(bad) if bad else "")
stall.close()

st = status_json()
check("no flash write refused", st["flash_refusals"] == 0, str(st["flash_refusals"]))
check("test mode left off", st["test_mode"] is False)

failed = [r for r in results if not r[1]]
print(f"== {len(results) - len(failed)}/{len(results)} passed")
sys.exit(1 if failed else 0)
