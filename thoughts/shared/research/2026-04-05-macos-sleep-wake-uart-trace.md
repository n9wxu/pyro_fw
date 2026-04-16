---
date: 2026-04-05T18:30:57-07:00
researcher: Joseph Julicher
git_commit: 442a2f1ceb4af3876d74430a66fd9476d19a0a6f
branch: main
repository: pyro_fw
topic: "UART trace of macOS sleep/wake USB event sequence — confirmed root cause of ping failure"
tags: [research, codebase, usb, ecm, tinyusb, macos, networking, suspend, resume, uart-trace]
status: complete
last_updated: 2026-04-05
last_updated_by: Joseph Julicher
---

# Research: macOS Sleep/Wake UART Trace — Confirmed Root Cause

**Date**: 2026-04-05
**Researcher**: Joseph Julicher
**Git Commit**: `442a2f1ceb4af3876d74430a66fd9476d19a0a6f`
**Branch**: main
**Repository**: pyro_fw

## Research Question

What USB-level events does macOS send to the pyro device during a system sleep/wake cycle, and which TinyUSB callbacks fire? Captured via a second computer monitoring UART (GPIO 0 TX, 115200 baud) to avoid the USB-to-UART adapter being killed by the same sleep event.

## Summary

**Root cause confirmed.** macOS sends a USB bus-level suspend on sleep and a bus-level resume on wake. It does **not** send `SET_INTERFACE alt=0/1` or `SET_CONFIGURATION`. As a result, `tud_network_init_cb()` never fires, `ecm_report(true)` (NETWORK_CONNECTION) is never sent, and the device never tells macOS that the ECM link is back up. Ping fails indefinitely after wake until the device is reset (which forces re-enumeration).

The prior `ecm_rndis_device.c` patch (always reinitialize on `SET_INTERFACE alt=1`) is correct but does not help here because macOS does not send `SET_INTERFACE` across a bus suspend/resume. A different fix is required: send `NETWORK_CONNECTION` proactively on `tud_resume_cb`.

---

## UART Trace

Captured on second computer. Firmware version built from commit `442a2f1` with working-tree changes to `net_glue.c` and `usb_descriptors.c` included.

```
$PYRO,37,...
$PYRO,38,...
$PYRO,39,...
$PYRO,40,...
$PYRO,41,...
$PYRO,42,...
!USB suspend          ← tud_suspend_cb fired; netif_set_link_down called
$PYRO,43,...          ← DMA UART TX drains queued messages; main loop still running
$PYRO,44,...
$PYRO,45,...
$PYRO,46,...
$PYRO,47,...
$PYRO,48,...
!USB resume           ← tud_resume_cb fired; netif_set_link_up called
$PYRO,49,...          ← main loop continues normally
$PYRO,50,...
...
$PYRO,62,...
[ping never recovers — reset required]
```

**Callbacks that fired:** `tud_suspend_cb`, `tud_resume_cb`

**Callbacks that did NOT fire:** `tud_network_init_cb`, `tud_mount_cb`, `tud_umount_cb`

**Control transfers that did NOT arrive:** `SET_INTERFACE alt=0`, `SET_INTERFACE alt=1`, `SET_CONFIGURATION`, `SET_ETHERNET_PACKET_FILTER`

---

## Detailed Findings

### 1. What macOS Does During Sleep/Wake

macOS sends a **USB bus-level suspend** on system sleep and a **USB bus-level resume** on wake. It does not deactivate the CDC-ECM data interface (`SET_INTERFACE alt=0/1`) and does not re-enumerate the device (`SET_CONFIGURATION`). This is a pure bus-level power event — the USB logical configuration is preserved across sleep.

This is consistent with the USB-to-UART adapter also losing connectivity during the same sleep event (both devices were on the same Mac USB bus).

### 2. What the Device Does

On suspend:
- `tud_suspend_cb` fires → `netif_set_link_down(&netif_data)` + UART log
- Main loop continues (DMA UART TX drains queued `$PYRO` strings)
- USB endpoint transfers are frozen by the bus suspend

On resume:
- `tud_resume_cb` fires → `netif_set_link_up(&netif_data)` + UART log
- Main loop resumes normally — `$PYRO` telemetry continues
- **`tud_network_init_cb()` is NOT called** — no endpoint re-arm, no mDNS restart
- **`ecm_report(true)` is NOT called** — NETWORK_CONNECTION notification is never sent to macOS

### 3. Why Ping Fails After Wake

After USB resume, the ECM data interface (alt=1) is still logically active — macOS never changed it. However, macOS's `IOUSBNetworkingFamily` driver has paused packet routing during the USB bus suspend. On wake, it expects the device to re-announce that the link is up via a `NETWORK_CONNECTION` CDC notification before it resumes routing.

The device never sends this notification because:
- `ecm_report(true)` is only called from the `SET_ETHERNET_PACKET_FILTER` handler (`ecm_rndis_device.c:286-289`)
- macOS does not re-send `SET_ETHERNET_PACKET_FILTER` after a bus-level resume (only after `SET_INTERFACE` cycling)
- `tud_resume_cb` in `net_glue.c` only calls `netif_set_link_up` — it does not trigger any CDC notification

### 4. Why Reset Recovers It

Pressing reset causes the RP2040 to reboot. TinyUSB re-initializes the USB hardware, causing a USB disconnect/reconnect on the bus. macOS detects the re-enumeration, goes through `SET_CONFIGURATION` → `SET_INTERFACE alt=1` → `SET_ETHERNET_PACKET_FILTER`, and the device responds with `NETWORK_CONNECTION`. Full recovery.

### 5. The Prior `ecm_rndis_device.c` Patch: Correct But Insufficient

The patch (moving `tud_network_init_cb()`, `can_xmit = true`, `tud_network_recv_renew()` outside the `ep_in == 0` guard) is correct for the `SET_INTERFACE` cycling scenario. But macOS does not send `SET_INTERFACE` across a bus suspend/resume — it only does so when the interface is administratively toggled (e.g., turning AirPort/Ethernet off and on). For sleep/wake, the patch has no effect.

### 6. Partial Re-evaluation of AI Claim

The prior research document (`2026-04-05-macos-ecm-network-connection-notification.md`) concluded the AI's diagnosis was wrong. This trace refines that:

| AI Claim | Assessment |
|---|---|
| "macOS sends no USB-level messages" | **Wrong** — macOS sends bus suspend/resume |
| "IOUSBNetworkingFamily waits for NETWORK_CONNECTION to resume routing" | **Likely correct** — this fits the observed behavior |
| "Device never sends NETWORK_CONNECTION because it only responds to SET_ETHERNET_PACKET_FILTER" | **Correct** |
| "Fix: proactively send NETWORK_CONNECTION" | **Correct direction** — wrong proposed mechanism (idle watchdog), but the right signal |

The AI correctly identified that NETWORK_CONNECTION needs to be sent proactively. It misidentified the trigger (it said no USB events; actually it's a bus suspend/resume). An idle watchdog is the wrong implementation — the right trigger is `tud_resume_cb`.

---

## Required Fix

`tud_resume_cb` needs to do more than just call `netif_set_link_up`. It needs to:

1. Re-announce the ECM link to macOS via `NETWORK_CONNECTION`
2. Ensure `can_xmit` is reset and the OUT endpoint is re-armed (in case the suspend interrupted an in-flight transfer)

`ecm_report()` is `static` inside `ecm_rndis_device.c` and not accessible from `net_glue.c`. A public API needs to be added to `ecm_rndis_device.c` (already a locally-patched file):

```c
/* New public API in ecm_rndis_device.c */
void tud_network_ecm_resume(void) {
    if (!_netd_itf.ecm_mode) return;
    if (_netd_itf.ep_in == 0) return;   /* not yet configured */
    can_xmit = true;
    tud_network_recv_renew();
    ecm_report(true);  /* NETWORK_CONNECTION: Connected */
}
```

Then in `net_glue.c`:

```c
void tud_resume_cb(void) {
    lwip_uart_printf("!USB resume\r\n");
    netif_set_link_up(&netif_data);
    tud_network_ecm_resume();   /* re-announce ECM link to macOS */
    if (mdns_started) {
        mdns_resp_restart(&netif_data);
    }
}
```

This is cleaner than the AI's idle watchdog because it fires exactly when needed (on USB resume) rather than polling.

---

## Code References

- `src/net_glue.c:157-168` — `tud_suspend_cb` / `tud_resume_cb` — currently only toggle lwIP link state
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:198-221` — `ecm_report()` static function — sends NETWORK_CONNECTION and CONNECTION_SPEED_CHANGE
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:284-289` — only current caller of `ecm_report(true)`: `SET_ETHERNET_PACKET_FILTER` handler
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:83` — `static bool can_xmit` — must be reset on resume
- `thoughts/shared/research/2026-04-05-macos-ecm-network-connection-notification.md` — prior analysis (partially superseded by this trace)

## Open Questions

- Does `can_xmit` actually become false during a suspend? If a transfer was in flight when the bus suspended, does the RP2040 USB hardware abort or preserve it on resume?
- Does `tud_network_recv_renew()` need to be called on resume or does the RP2040 USB controller automatically re-arm the OUT endpoint after resume?
- After sending `NETWORK_CONNECTION` from `tud_resume_cb`, does macOS also re-send `SET_ETHERNET_PACKET_FILTER`? If so, `ecm_report(true)` would be called twice — once from `tud_resume_cb` and once from the filter handler. This should be harmless but worth confirming.
