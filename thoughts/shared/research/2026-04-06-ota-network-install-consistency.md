---
date: 2026-04-06T00:00:00-07:00
researcher: Joseph Julicher
git_commit: 442a2f1ceb4af3876d74430a66fd9476d19a0a6f
branch: main
repository: pyro_fw
topic: "OTA mode and network file writes inconsistency — install.py device discovery, picotool BOOTSEL, and www POST"
tags: [research, install, ota, picotool, usb, ecm, rndis, http-server, www-upload, network, bootsel]
status: complete
last_updated: 2026-04-06
last_updated_by: Joseph Julicher
---

# Research: OTA Mode and Network File Writes Inconsistency

**Date**: 2026-04-06  
**Researcher**: Joseph Julicher  
**Git Commit**: `442a2f1ceb4af3876d74430a66fd9476d19a0a6f`  
**Branch**: main  
**Repository**: n9wxu/pyro_fw

## Research Question

The OTA mode and network file writes are not consistent:
1. The device is not being found on the network so install.py falls back to BOOTSEL modes.
2. PICOTOOL does not force the device into BOOTSEL mode. (Is the picotool not built into the USB configuration?)
3. POST of the web pages is not consistently happening but this may be related to failing to be found on the network.

---

## Summary

All three symptoms trace back to a single root condition: **ping to 192.168.7.1 is broken at the current HEAD (v2.1.41)**, which was documented in the ping regression investigation (`2026-04-05-ping-regression-root-cause.md`). Because `install.py` uses `ping -c 1 -t 2 192.168.7.1` as its sole device-detection mechanism, the script always falls into the "No device found" branch even when the device is USB-enumerated and DHCP-functional.

The picotool issue is independent: the Pico SDK reset interface IS compiled into the firmware (class 0xFF/0x00/0x01 on USB interface 2 in both RNDIS and ECM configurations), so picotool *should* be able to trigger BOOTSEL via USB control request. However, the `picotool reboot -u -f` command specifically requires the USB device to be enumerated and accessible by picotool — if the device is not presenting on USB (not running, crashed, or host USB driver hasn't bound), picotool returns non-zero and install.py prompts manual BOOTSEL.

The `/www/` POST failures are downstream of issue 1: once the device is not detected on the network, no OTA or www POST is attempted by the normal install flow. The POST mechanism itself is straightforward and should work when the network is reachable.

---

## Detailed Findings

### Issue 1: Device Not Found on Network — `install.py` Network Detection

**Detection mechanism** (`support/install.py:42–44, 102`):
```python
def ping(host):
    _, rc = run(["ping", "-c", "1", "-t", "2", host])
    return rc == 0

device_up = ping(HOST)   # HOST = "192.168.7.1"
```

`install.py` calls `ping -c 1 -t 2 192.168.7.1` at startup. If ping returns non-zero, `device_up = False` and the script goes directly to the BOOTSEL/picotool fallback menu (line 113–120).

**Known state of ping at v2.1.41**: Ping (ICMP echo) is broken since at least commit `1116373` ("DMA UART TX replaces polled ring buffer", v2.1.29). DHCP negotiation still works — the host receives a 192.168.7.x lease — but ICMP echo replies are not being sent. This is documented in `thoughts/shared/research/2026-04-05-ping-regression-root-cause.md`.

**Network interface in firmware** (`src/net_glue.c:36–38`):
```c
static const ip4_addr_t ipaddr  = INIT_IP4(192, 168, 7, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);
```

The device is a static 192.168.7.1 on a USB ECM (macOS/Linux) or RNDIS (Windows) interface. The host gets DHCP from the device's on-board dhserver at 192.168.7.2 or 192.168.7.3.

`lwipopts.h:10` has `LWIP_ICMP 1` — ICMP is compiled in. The lwIP stack is expected to generate echo replies automatically. The failure is believed to be in the ICMP packet path being starved because of the DMA UART TX changes; see the ping regression research document for details.

**Consequence for install.py**: Even a fully-running, DHCP-functional device at HEAD will be reported as "not found" by install.py. The OTA and www-upload paths (install.py lines 167–237) are never reached.

---

### Issue 2: Picotool Cannot Force BOOTSEL

#### Is the picotool/reset interface compiled in?

**Yes.** Both USB configurations include a Pico SDK reset interface as USB interface 2:

- RNDIS config (`usb_descriptors.c:62–66`): 3 interfaces — RNDIS control/data + `TUD_RPI_RESET_DESCRIPTOR(2, ...)`
- ECM config (`usb_descriptors.c:70–74`): 3 interfaces — ECM control/data + `TUD_RPI_RESET_DESCRIPTOR(2, ...)`

The descriptor macro (`usb_descriptors.c:34–36`):
```c
#define TUD_RPI_RESET_DESCRIPTOR(_itfnum, _stridx) \
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, \
  RESET_INTERFACE_SUBCLASS, RESET_INTERFACE_PROTOCOL, _stridx
```

From the Pico SDK 2.2.0 headers: `RESET_INTERFACE_SUBCLASS = 0x00`, `RESET_INTERFACE_PROTOCOL = 0x01`. The class/subclass/protocol triplet is `0xFF / 0x00 / 0x01`, which is exactly what picotool targets with the `reboot -u -f` command when given `--vid 0x2E8A --pid 0x4002`.

Note: This is **not** the PICOBOOT interface from the RP2040 bootrom (that uses a different class identifier). This is the Pico SDK application-level reset interface. Picotool supports both.

#### The reset interface driver

`src/reset_interface.c` implements a custom TinyUSB app driver registered via `usbd_app_driver_get_cb`:

- `resetd_open()` (`reset_interface.c:19–27`): matches class 0xFF / RESET_INTERFACE_SUBCLASS / RESET_INTERFACE_PROTOCOL
- `resetd_control_xfer_cb()` (`reset_interface.c:29–43`): on `RESET_REQUEST_BOOTSEL`, sets `pending_reset = 1`; on `RESET_REQUEST_FLASH`, sets `pending_reset = 2`
- The actual bootrom call is **deferred to the main loop** to avoid blocking the USB stack

#### install.py picotool invocation (`support/install.py:209`):
```python
run([picotool, "reboot", "-u", "-f", "--vid", "0x2E8A", "--pid", "0x4002"])
```

`-u` = reboot to BOOTSEL/USBBOOT mode. `-f` = force (send reset request to the running app, don't require it to already be in BOOTSEL). The VID/PID matches `usb_descriptors.c:7–8`.

#### Why picotool might fail:

The `run()` function (`install.py:33–41`) uses `subprocess.run` with `timeout=30`. If picotool exits non-zero (rc != 0), install.py line 211 falls back to prompting manual BOOTSEL. Possible causes for non-zero exit:

1. **Device not enumerated on USB**: If the firmware crashed, never booted, or USB driver hasn't bound, picotool can't find the device by VID/PID.
2. **USB host driver conflict**: On macOS, the RNDIS interface may not bind cleanly. If the host selected RNDIS (config 0) and the RNDIS driver is in a bad state, USB access to interface 2 may fail.
3. **picotool not in PATH / wrong version**: install.py searches `["picotool", "~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool"]` — if neither exists, `run()` returns `("command not found", 127)` and picotool is skipped entirely (line 199–202), not falling to manual BOOTSEL but returning early with "Error: picotool not found".
4. **Firmware doesn't return to main loop**: If `hal_platform_service()` is stalled (e.g., busy-waiting in DMA UART TX path at `hal_hardware.c`), `pending_reset` is never acted on and the device does not reboot, but picotool itself returns 0 (the control request succeeded). In that case, install.py proceeds to `picotool load` without the device actually being in BOOTSEL mode, and the load fails.

---

### Issue 3: `/www/` POST Not Consistently Happening

#### install.py www upload flow

`upload_www()` (`install.py:58–74`) is called only *after* `wait_for_device()` returns `True`. `wait_for_device()` polls `ping` for up to 20–60 seconds. If ping never succeeds (which is the current state at HEAD), `wait_for_device()` returns `False` and `upload_www()` is never called.

#### The `/www/{name}` POST handler (when network IS reachable)

**Location**: `src/http_server.c:489–553`

Flow:
1. `conn_alloc()` — gets a slot from the 8-slot `conn_pool`. If pool full, connection closed.
2. `lfs_mount()` — mounts littlefs. A new mount happens per file upload.
3. `lfs_mkdir("/www")` — creates the `/www` directory if absent (error ignored).
4. `lfs_file_open(path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)` — opens/creates/truncates the target file.
5. Sets `CONN_RECEIVING_FILE` phase and `remaining = content_length`.
6. Streams body in 512-byte chunks via `lfs_file_write()` across multiple TCP `on_recv` callbacks.
7. On final byte: `conn_free()` (closes file, unmounts lfs), sends `HTTP/1.1 201 Created`.

`install.py:63–73` sends each file with `urllib.request.urlopen(req, timeout=10)`. The 10-second per-file timeout is the constraint. Each request must complete lfs mount + file write + lfs unmount within 10 seconds.

The `conn_state_t` pool is 8 slots (`http_server.c:44`). If install.py posts multiple files in rapid succession without waiting for each ACK, all 8 slots could be consumed. However, `urllib.request.urlopen` is synchronous — it waits for the 201 response before returning — so uploads are effectively serialized.

#### Possible sources of inconsistency in `/www/` POST (independent of ping):

- **Header parsing cap** (`http_server.c:334`): First-packet header is copied into a 512-byte stack buffer. If HTTP headers exceed 511 bytes, `Content-Length` can be misread. With `urllib.request`, headers are typically short, so this is unlikely to be a problem.
- **`lfs_mount` concurrency**: Only one `conn_state_t` can hold an open lfs mount (lfs does not support concurrent mounts). If two `/www/` uploads are in-flight simultaneously — which cannot happen with the serialized `urllib.request` loop — this would corrupt state.
- **`tcp_recved()` pacing**: The server calls `tcp_recved(pcb, p->tot_len)` (line 288) after each pbuf, keeping the receive window open. No backpressure is applied to the client.
- **Device reboot after OTA**: In the OTA+www sequence (`install.py:170–182`), the script does `time.sleep(3)` then `wait_for_device(timeout=60)` after the OTA post. If the device comes back up but ping still fails (regression), `upload_www()` is again skipped.

---

## Architecture Documentation

### Overall install.py Decision Tree

```
ping 192.168.7.1
├── SUCCESS (device_up = True)
│   ├── Option 1: OTA → POST /api/ota → wait for reboot → ping → upload_www
│   ├── Option 2: Web files only → upload_www
│   └── Option 3: picotool flash → picotool reboot -u -f → picotool load → reboot → ping → upload_www
└── FAILURE (device_up = False)
    ├── Option 1: BOOTSEL drag-and-drop → copy UF2 to /Volumes/RPI-RP2 → ping → upload_www
    └── Option 2: picotool → picotool reboot -u -f → picotool load → reboot → ping → upload_www
```

All paths culminate in `wait_for_device()` + `upload_www()`. Both require ping to succeed.

### USB Reset Interface Path

```
picotool reboot -u -f --vid 0x2E8A --pid 0x4002
  → USB control request RESET_REQUEST_BOOTSEL → interface 2 (class 0xFF/0x00/0x01)
  → resetd_control_xfer_cb() sets pending_reset = 1
  → main loop: hal_platform_service() → pending_reset check
  → reset_usb_boot(0, 0)  [bootrom BOOTSEL entry]
```

### Network Stack Bring-Up at Boot

```
hal_platform_init()
  → tud_init()         — USB device stack
  → net_init()         — lwip_init() + netif_add(192.168.7.1)
  → net_start()        — spins until netif_is_up (immediate, flags set statically)
                       → dhserver_init(192.168.7.2–3) + dns_init()
  → http_server_init() — tcp_listen(port 80, backlog 8)

hal_platform_service() [main loop]
  → tud_task()         — USB frame pump, calls tud_network_recv_cb on incoming frames
  → net_service()      — ethernet_input() + sys_check_timeouts()
  → net_mdns_poll()    — one-time mDNS register (hostname "pyro", service "_pyro._tcp")
```

### OTA Flash Write Path

```
POST /api/ota (content_length bytes of .bin)
  → pfb_firmware_commit()             — marks current firmware as good
  → ota_offset=0, ota_buf_fill=0      — reset write cursor
  → streaming receive:
      pbuf chunks → ota_write(buf, len)
          → accumulate in static ota_buf[4096]
          → when full: flash_range_erase + flash_range_program (4096-byte sector)
  → final: ota_flush() + pfb_mark_download_slot_as_valid()
  → tcp_write("OTA OK, rebooting...") + tcp_output()
  → pfb_perform_update() — watchdog reboot (immediate, TCP FIN not guaranteed)
```

---

## Code References

- `support/install.py:42–44` — `ping()` function using `ping -c 1 -t 2`
- `support/install.py:58–74` — `upload_www()` using `urllib.request` with 10s timeout
- `support/install.py:76–85` — `wait_for_device()` polling loop
- `support/install.py:102` — `device_up = ping(HOST)` — the single branch point
- `support/install.py:197–211` — picotool search and `reboot -u -f` invocation
- `src/usb_descriptors.c:7–8` — `USB_VID 0x2E8A`, `USB_PID 0x4002`
- `src/usb_descriptors.c:34–36` — `TUD_RPI_RESET_DESCRIPTOR` macro definition
- `src/usb_descriptors.c:62–74` — RNDIS and ECM configuration descriptors
- `src/reset_interface.c:19–43` — reset interface driver: `resetd_open`, `resetd_control_xfer_cb`
- `src/tusb_config.h:50` — `CFG_TUD_ECM_RNDIS 1`
- `src/net_glue.c:36–38` — static IP 192.168.7.1 definition
- `src/net_glue.c:41–42` — DHCP pool: 192.168.7.2 and 192.168.7.3
- `src/lwipopts.h:10` — `LWIP_ICMP 1`
- `src/http_server.c:34–42` — `conn_state_t` struct (8-slot pool)
- `src/http_server.c:489–553` — `/www/{name}` POST handler
- `src/http_server.c:442–487` — `/api/ota` POST handler, single-packet path
- `src/http_server.c:303–331` — `/api/ota` continuation packet path

---

## Related Research

- `thoughts/shared/research/2026-04-05-ping-regression-root-cause.md` — root cause investigation of ICMP regression since v2.1.29
- `thoughts/shared/research/2026-04-05-macos-ecm-network-connection-notification.md` — ECM NETWORK_CONNECTION notification on macOS sleep/wake
- `thoughts/shared/research/2026-04-06-www-post-upload-flow.md` — prior research on www POST mechanics
- `thoughts/shared/plans/2026-04-05-usb-sleep-wake-ecm-resume.md` — ecm_resume_pending fix plan (untested)

---

## Open Questions

1. **Root cause of ping regression**: Is the DMA UART TX path (`hal_hardware.c`) consuming cycles in a way that starves `net_service()` from calling `sys_check_timeouts()`, preventing ICMP reply generation? The working tree has uncommitted changes to `src/http_server.c`, `src/flight_states.c/h`, `src/pressure_processing.c/h` that may affect this.

2. **picotool failure mode**: When picotool returns non-zero for `reboot -u -f`, does it fail to *find* the device (USB not enumerated), or does it find the device but the reset interface claim fails? The error output from `run([picotool, "reboot", ...])` is captured in `out` (line 210) but only printed if `rc != 0` — what does the actual error message say?

3. **install.py detection alternative**: Since DHCP works even when ping is broken, is there another way to confirm the device is up — e.g., checking for the `RPI-RP2` volume, or attempting an HTTP GET to `/api/status` instead of ping?

4. **Concurrent install.py + www upload**: `upload_www()` opens a new TCP connection per file with 10s timeout. If lfs_mount takes longer than expected during a file write, would the timeout fire and leave a half-written file?
