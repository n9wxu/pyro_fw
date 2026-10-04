#!/usr/bin/env python3
"""Update a Pyro board's firmware from a GitHub release [WEB-UI-05, OTA-01].

Usage:
  ./update_from_release.py                  Check and update from the latest release
  ./update_from_release.py --check          Check only, don't update
  ./update_from_release.py --version 1.2.0  Update to a specific version
  ./update_from_release.py --host IP        The board's address

The image is the release's fw_<board>_fota.bin for the board the device
reports: the boards do not take each other's firmware. Releases up to v2.2.0
carried one unqualified image, LEGACY_ASSET_NAME, tried only when the release
has none for the board. Exits non-zero unless the board comes back running
the version asked for.
"""
import argparse
import json
import os
import sys
import tempfile
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pyro_http import BoardError, board_id_of, get_json, post, wait_down, wait_up  # noqa: E402

REPO = "n9wxu/pyro_fw"
API_URL = f"https://api.github.com/repos/{REPO}/releases"
LEGACY_ASSET_NAME = "pyro_fw_c_fota_image.bin"


def asset_name_for(board):
    return f"fw_{board}_fota.bin"


def github(url):
    """The JSON at url, or None (404: no such release)."""
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise SystemExit(f"Error: GitHub answered HTTP {e.code} for {url}")
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise SystemExit(f"Error: cannot fetch {url}: {e}")


def find_release(version=None, include_beta=False):
    """The release to install: the one tagged v<version>, or the newest, a
    prerelease included with include_beta."""
    if version:
        return github(f"{API_URL}/tags/v{version}")
    if not include_beta:
        return github(f"{API_URL}/latest")
    page = 1
    while True:
        releases = github(f"{API_URL}?per_page=100&page={page}") or []
        for r in releases:
            if not r.get("draft"):
                return r
        if len(releases) < 100:
            return None
        page += 1


def find_asset(release, board):
    """The OTA image for board, else the pre-v2.2.0 image, else (None, 0)."""
    assets = release.get("assets", [])
    for want in (asset_name_for(board), LEGACY_ASSET_NAME):
        for asset in assets:
            if asset["name"] == want:
                if want == LEGACY_ASSET_NAME:
                    print(f"Note: no {asset_name_for(board)} in this release; using {LEGACY_ASSET_NAME}")
                return asset["browser_download_url"], asset["size"]
    return None, 0


def download_asset(url, size):
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
    try:
        with urllib.request.urlopen(url, timeout=60) as resp:
            downloaded = 0
            while True:
                chunk = resp.read(8192)
                if not chunk:
                    break
                tmp.write(chunk)
                downloaded += len(chunk)
                pct = (downloaded * 100) // size if size else 0
                print(f"\r  Downloading: {downloaded}/{size} bytes ({pct}%)", end="", flush=True)
        print()
        tmp.close()
        if size and downloaded != size:
            raise OSError(f"{downloaded} of {size} bytes")
        return tmp.name
    except (urllib.error.URLError, OSError) as e:
        tmp.close()
        os.unlink(tmp.name)
        print(f"\nError downloading: {e}")
        return None


def push_ota(host, filepath):
    """True once the board has taken the image and begun to restart."""
    with open(filepath, "rb") as f:
        data = f.read()
    print(f"  Uploading {len(data)} bytes to {host}...")
    try:
        print("  " + post(host, "/api/ota", data, timeout=120).strip())
    except BoardError as e:
        print(f"  ✗ the board refused the image: {e}")
        return False
    if not wait_down(host, timeout_s=20):
        print("  ✗ the board did not restart")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="Update a Pyro board from GitHub releases")
    ap.add_argument("--host", default="192.168.7.1", help="Device address")
    ap.add_argument("--board", help="Override the board the device reports (mk1a, mk1b, mk1c)")
    ap.add_argument("--check", action="store_true", help="Check only, don't update")
    ap.add_argument("--version", help="Update to specific version (e.g. 1.2.0)")
    ap.add_argument("--force", action="store_true", help="Update even if same version")
    ap.add_argument("--beta", action="store_true", help="Include beta/prerelease versions")
    args = ap.parse_args()

    print(f"Pyro firmware update\nDevice: {args.host}\n")
    status = get_json(args.host)
    if not status:
        print(f"Error: no answer from {args.host}")
        return 1
    current = status.get("fw_version", "unknown")
    reported = board_id_of(status)
    if args.board and reported and args.board != reported:
        print(f"Refused: the board is {reported}, not {args.board}")
        return 1
    board = args.board or reported
    if not board:
        print("Error: the device did not report its board; name it with --board.")
        return 1
    print(f"Device board: {board}\nCurrent firmware: v{current}")

    print(f"Checking GitHub releases ({REPO})...")
    release = find_release(args.version, args.beta)
    if not release:
        print(f"No release found ({'v' + args.version if args.version else 'any'})")
        return 1
    release_ver = release.get("tag_name", "").lstrip("v")
    print(f"Release: v{release_ver}")

    if current == release_ver and not args.force:
        print(f"\n✓ Already up to date (v{current})")
        return 0
    if args.check:
        print(f"\nUpdate available: v{current} → v{release_ver}")
        return 0

    asset_url, asset_size = find_asset(release, board)
    if not asset_url:
        print(f"Error: {asset_name_for(board)} not found in v{release_ver}")
        return 1

    print(f"\nUpdating: v{current} → v{release_ver}")
    filepath = download_asset(asset_url, asset_size)
    if not filepath:
        return 1
    try:
        if not push_ota(args.host, filepath):
            return 1
    finally:
        os.unlink(filepath)

    print("  Waiting for the board...", flush=True)
    status = wait_up(args.host, timeout_s=60)
    if not status:
        print("\n✗ The board did not come back; check it by hand.")
        return 1
    new_ver = status.get("fw_version")
    if new_ver != release_ver:
        print(f"\n✗ The board runs v{new_ver}, not v{release_ver}: the image did not take, or it rolled back.")
        return 1
    print(f"\n✓ Updated to v{new_ver}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
