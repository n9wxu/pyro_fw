#!/usr/bin/env python3
"""Update Pyro firmware from GitHub releases.

Usage:
  ./update_from_release.py                  Check and update from latest release
  ./update_from_release.py --check          Check only, don't update
  ./update_from_release.py --version 1.2.0  Update to specific version
  ./update_from_release.py --host IP        Specify device address
"""
import argparse, json, sys, time, os, tempfile

try:
    import urllib.request
    import urllib.error
except ImportError:
    print("Error: Python urllib required")
    sys.exit(1)

REPO = "n9wxu/pyro_fw"  # Update to your GitHub repo
API_URL = f"https://api.github.com/repos/{REPO}/releases"

# A release carries one OTA image per supported board, named after the
# board, because the boards do not take each other's firmware -- mk1b is a
# 2 MB part and mk1a and mk1c are 16 MB, so the flash layouts differ. The
# asset is chosen from the board the DEVICE reports, never guessed.
#
# Releases up to v2.2.0 shipped a single unqualified image built for the
# default board. LEGACY_ASSET_NAME is tried only as a fallback, so this
# script still updates from them.
def asset_name_for(board):
    return f"fw_{board}_fota.bin"


# Releases up to and including v2.2.0 shipped a single unqualified image.
LEGACY_ASSET_NAME = "pyro_fw_c_fota_image.bin"

parser = argparse.ArgumentParser(description="Update Pyro MK1B from GitHub releases")
parser.add_argument("--host", default="192.168.7.1", help="Device address")
parser.add_argument("--board", help="Override the board the device reports (mk1a, mk1b, mk1c)")
parser.add_argument("--check", action="store_true", help="Check only, don't update")
parser.add_argument("--version", help="Update to specific version (e.g. 1.2.0)")
parser.add_argument("--force", action="store_true", help="Update even if same version")
parser.add_argument("--beta", action="store_true", help="Include beta/prerelease versions")
args = parser.parse_args()


def get_device_status(host):
    """Get /api/status from the device, or None if it cannot be reached."""
    try:
        url = f"http://{host}/api/status"
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read().decode())
    except Exception as e:
        print(f"Error: Cannot reach device at {host}: {e}")
        return None


def board_id_of(status):
    """The board token from /api/status.

    board_id is the token the asset names use. Firmware older than it
    reported only the display name, so that is reduced as a fallback --
    "Pyro MK1A" to "mk1a" -- strictly, so a decorated name yields None
    instead of a guess.
    """
    if not status:
        return None
    if status.get("board_id"):
        return str(status["board_id"])
    name = str(status.get("board", "")).strip()
    if not name:
        return None
    last = name.split()[-1].lower()
    return last if last.isalnum() else None


def get_device_version(host):
    """Get current firmware version from device."""
    status = get_device_status(host)
    return status.get("fw_version", "unknown") if status else None


def get_releases():
    """Fetch releases from GitHub API."""
    try:
        req = urllib.request.Request(API_URL, headers={"Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode())
    except Exception as e:
        print(f"Error: Cannot fetch releases: {e}")
        return None


def find_release(releases, version=None, include_beta=False):
    """Find a release by version, or return latest."""
    for r in releases:
        if r.get("draft"):
            continue
        if r.get("prerelease") and not include_beta:
            continue
        tag = r.get("tag_name", "")
        ver = tag.lstrip("v")
        if version and ver == version:
            return r, ver
        if not version:
            return r, ver
    return None, None


def find_asset(release, board=None):
    """The OTA image for this board, falling back to the pre-v2.2.0 name.

    Returning the legacy asset when a board-specific one exists would risk
    writing another board's firmware, so the board-specific name wins and
    the fallback applies only when nothing matches it.
    """
    assets = release.get("assets", [])
    if board:
        want = asset_name_for(board)
        for asset in assets:
            if asset["name"] == want:
                return asset["browser_download_url"], asset["size"]
    for asset in assets:
        if asset["name"] == LEGACY_ASSET_NAME:
            if board:
                print(f"Note: no {asset_name_for(board)} in this release; "
                      f"falling back to {LEGACY_ASSET_NAME}")
            return asset["browser_download_url"], asset["size"]
    return None, 0


def download_asset(url, size):
    """Download asset to temp file with progress."""
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=60) as resp:
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
        return tmp.name
    except Exception as e:
        tmp.close()
        os.unlink(tmp.name)
        print(f"\nError downloading: {e}")
        return None


def push_ota(host, filepath):
    """Push firmware to device via OTA."""
    size = os.path.getsize(filepath)
    print(f"  Uploading {size} bytes to {host}...")
    with open(filepath, "rb") as f:
        data = f.read()
    try:
        req = urllib.request.Request(
            f"http://{host}/api/ota",
            data=data,
            headers={"Content-Type": "application/octet-stream"},
            method="POST"
        )
        urllib.request.urlopen(req, timeout=120)
        return True
    except urllib.error.URLError:
        # Expected — device reboots mid-response
        return True
    except Exception as e:
        print(f"  Error: {e}")
        return False


def wait_for_device(host, timeout=30):
    """Wait for device to come back after reboot."""
    print(f"  Waiting for device...", end="", flush=True)
    for i in range(timeout):
        time.sleep(1)
        print(".", end="", flush=True)
        try:
            ver = get_device_version(host)
            if ver:
                print(f" up! (v{ver})")
                return ver
        except:
            pass
    print(" timeout")
    return None


def main():
    print(f"Pyro MK1B Firmware Update")
    print(f"Device: {args.host}\n")

    # Get current device version
    status = get_device_status(args.host)
    current = status.get("fw_version", "unknown") if status else None
    board = args.board or (board_id_of(status) if status else None)
    if board:
        print(f"Device board: {board}")
    elif not args.check:
        print("Warning: the device did not report its board; the OTA image "
              "cannot be matched to it. Pass --board to choose explicitly.")
    if current is None:
        sys.exit(1)
    print(f"Current firmware: v{current}")

    # Fetch releases
    print(f"Checking GitHub releases ({REPO})...")
    releases = get_releases()
    if not releases:
        sys.exit(1)

    release, release_ver = find_release(releases, args.version, args.beta)
    if not release:
        target = f"v{args.version}" if args.version else "any"
        print(f"No release found ({target})")
        sys.exit(1)

    print(f"Latest release: v{release_ver} ({release['tag_name']})")

    # Compare versions
    if current == release_ver and not args.force:
        print(f"\n✓ Already up to date (v{current})")
        sys.exit(0)

    if args.check:
        print(f"\nUpdate available: v{current} → v{release_ver}")
        sys.exit(0)

    # Find OTA asset
    asset_url, asset_size = find_asset(release, board)
    if not asset_url:
        wanted = asset_name_for(board) if board else LEGACY_ASSET_NAME
        print(f"Error: {wanted} not found in release assets")
        sys.exit(1)

    print(f"\nUpdating: v{current} → v{release_ver}")

    # Download
    filepath = download_asset(asset_url, asset_size)
    if not filepath:
        sys.exit(1)

    try:
        # Push OTA
        if not push_ota(args.host, filepath):
            sys.exit(1)

        # Wait for reboot
        new_ver = wait_for_device(args.host)
        if new_ver == release_ver:
            print(f"\n✓ Updated to v{new_ver}")
        elif new_ver:
            print(f"\n⚠ Device reports v{new_ver}, expected v{release_ver}")
        else:
            print(f"\n⚠ Device did not come back — check manually")
    finally:
        os.unlink(filepath)


if __name__ == "__main__":
    main()
