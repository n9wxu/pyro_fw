"""Reaching a board over its USB network, for the support scripts.

A POST carries X-Pyro: 1, which the board requires of every POST
(http_origin_refusal() in src/http_conn.c). A refusal (HTTP 400 and up) or no
answer raises BoardError, so a failure is never taken for success.
"""
import glob
import json
import os
import platform
import shutil
import subprocess
import time
import urllib.error
import urllib.request

POST_HEADERS = {"X-Pyro": "1"}


class BoardError(Exception):
    pass


def post(host, path, data=b"", content_type="application/octet-stream", timeout=10):
    """POST and return the body. Raises BoardError on a refusal or no answer."""
    headers = dict(POST_HEADERS, **{"Content-Type": content_type})
    req = urllib.request.Request(f"http://{host}{path}", data=data, method="POST", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:  # before URLError, which it subclasses
        raise BoardError(f"HTTP {e.code}: {e.read().decode(errors='replace').strip()}") from e
    except (urllib.error.URLError, OSError) as e:
        raise BoardError(f"no answer from {host}: {e}") from e


def get_json(host, path="/api/status", timeout=5):
    """The JSON at path, or None when the board does not answer with it."""
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def board_id_of(status):
    """The board token from /api/status. Firmware without board_id reports only
    the display name, reduced strictly ("Pyro MK1A" to "mk1a"): a decorated
    name yields None rather than a guess."""
    if not status:
        return None
    if status.get("board_id"):
        return str(status["board_id"])
    name = str(status.get("board", "")).strip()
    if not name:
        return None
    last = name.split()[-1].lower()
    return last if last.isalnum() else None


def ping(host, wait_s=2):
    """One echo request. The reply timeout is -W on Linux, where -t is the
    TTL, and -t on macOS and the BSDs."""
    system = platform.system()
    if system == "Windows":
        cmd = ["ping", "-n", "1", "-w", str(wait_s * 1000), host]
    elif system == "Linux":
        cmd = ["ping", "-c", "1", "-W", str(wait_s), host]
    else:
        cmd = ["ping", "-c", "1", "-t", str(wait_s), host]
    try:
        return subprocess.run(cmd, capture_output=True, timeout=wait_s + 3).returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def wait_down(host, timeout_s=20):
    """True once the board stops answering: the reboot has begun."""
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        if get_json(host, timeout=1) is None:
            return True
        time.sleep(0.5)
    return False


def wait_up(host, timeout_s=60):
    """/api/status once the board answers again, or None."""
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        status = get_json(host, timeout=2)
        if status:
            return status
        time.sleep(1)
    return None


def find_picotool():
    """picotool on PATH, else the newest the VS Code Pico extension installed."""
    found = shutil.which("picotool")
    if found:
        return found
    for p in sorted(glob.glob(os.path.expanduser("~/.pico-sdk/picotool/*/picotool/picotool")), reverse=True):
        if os.access(p, os.X_OK):
            return p
    return None
