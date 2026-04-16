# USB Sleep/Wake ECM Resume Implementation Plan

## Overview

After macOS system sleep/wake, ping to the device fails indefinitely until a hardware reset.
The root cause is confirmed: macOS sends a USB bus-level suspend/resume (not `SET_INTERFACE`)
so `tud_network_init_cb()` and `ecm_report(true)` never fire, and macOS's
`IOUSBNetworkingFamily` never receives the `NETWORK_CONNECTION` CDC notification it needs to
resume packet routing.

The fix adds a public `tud_network_ecm_resume()` API to the locally-patched TinyUSB ECM
driver and calls it (along with an mDNS restart) from `tud_resume_cb()`.

---

## Current State Analysis

**What the UART trace showed** (commit `442a2f1`, research `2026-04-05-macos-sleep-wake-uart-trace.md`):

- `tud_suspend_cb` fires on sleep → `netif_set_link_down` called ✓
- `tud_resume_cb` fires on wake → `netif_set_link_up` called — **but nothing more**
- `tud_network_init_cb()` does NOT fire (no `SET_INTERFACE alt=1`)
- `ecm_report(true)` does NOT fire (no `SET_ETHERNET_PACKET_FILTER`)
- macOS never receives `NETWORK_CONNECTION` → routing stays paused → ping fails

**Why `ecm_report()` is inaccessible:**
`ecm_report()` is `static` in `ecm_rndis_device.c:198`. There is no public API in
`net_device.h` to trigger it from outside the driver.

**Key driver state after resume:**
- `can_xmit`: may be `false` if a TX was in-flight when the bus suspended and the
  `ep_in` completion callback never fired
- OUT endpoint pending transfer: the RP2040 USB controller may or may not have preserved
  the `tud_network_recv_renew()` transfer across suspend — re-arming is safe because
  TinyUSB's `usbd_edpt_xfer()` is a no-op if the endpoint is already active

**Current `tud_resume_cb` in `net_glue.c:164-168`:**
```c
void tud_resume_cb(void) {
    lwip_uart_printf("!USB resume\r\n");
    netif_set_link_up(&netif_data);
}
```

**Prior `ecm_rndis_device.c` patch (already in place):** The `SET_INTERFACE alt=1`
handler always calls `tud_network_init_cb()`, `can_xmit = true`, and
`tud_network_recv_renew()`. This correctly handles interface cycling — but macOS does not
send `SET_INTERFACE` across a bus-level sleep/wake, so the patch has no effect here.

---

## Desired End State

After macOS system sleep/wake, `ping 192.168.7.1` recovers automatically within a few
seconds of wake, without a hardware reset or USB replug. `pyro.local` also re-resolves.

### Verification:
- `ping 192.168.7.1` is running before sleep. Mac sleeps. Mac wakes. Within ~5 seconds,
  ping succeeds again without any manual intervention.
- UART log shows: `!USB suspend` on sleep, `!USB resume` on wake, followed by resumed
  `$PYRO` telemetry.
- No hardware reset required.

---

## What We're NOT Doing

- No idle watchdog or timer-based NETWORK_CONNECTION polling.
- No changes to the DHCP/DNS server logic.
- No changes to the `SET_INTERFACE` handler or the existing `ecm_rndis_device.c` patch.
- No changes to `tud_mount_cb`, `tud_umount_cb`, or `tud_suspend_cb`.
- No changes to the mDNS hostname/suffix logic.

---

## Implementation Approach

Three coordinated changes across two files (plus one header):

1. `ecm_rndis_device.c` — add `tud_network_ecm_resume()` public function
2. `net_device.h` — declare `tud_network_ecm_resume()` in the Application API section
3. `net_glue.c` — call `tud_network_ecm_resume()` and `mdns_resp_restart()` from `tud_resume_cb()`

---

## Phase 1: Add `tud_network_ecm_resume()` to the ECM Driver

### Overview

Add a new public function to the locally-patched TinyUSB ECM/RNDIS driver that re-arms the
transmit path and sends the `NETWORK_CONNECTION` notification. This is the driver-side
half of the fix.

### Changes Required

#### 1.1 New function in `ecm_rndis_device.c`

**File**: `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c`
**Location**: After `ecm_report()` (after line 221), before `netd_control_xfer_cb`

```c
/* Called from tud_resume_cb() to re-announce the ECM link to macOS after a
 * USB bus-level suspend/resume.  macOS does not re-send SET_INTERFACE alt=1 or
 * SET_ETHERNET_PACKET_FILTER across a bus suspend — it expects the device to
 * proactively send NETWORK_CONNECTION before it resumes routing. */
void tud_network_ecm_resume(void) {
    if (!_netd_itf.ecm_mode) return;
    if (_netd_itf.ep_in == 0) return;   /* not yet configured — nothing to do */
    can_xmit = true;                    /* reset in case suspend interrupted a TX */
    tud_network_recv_renew();           /* re-arm OUT endpoint */
    ecm_report(true);                   /* NETWORK_CONNECTION: Connected */
}
```

**Why each line:**
- `!_netd_itf.ecm_mode` guard: RNDIS does not use `ecm_report()` at all; skip it
- `ep_in == 0` guard: endpoints have not been opened yet (device not yet configured on
  this boot); calling `ecm_report()` would try to send on ep_notif which may be 0 too
- `can_xmit = true`: mirrors what the `SET_INTERFACE alt=1` handler does; safe to set
  unconditionally because if can_xmit was already true, this is a no-op for behavior
- `tud_network_recv_renew()`: re-arms the OUT bulk endpoint in case the suspend
  disrupted an in-flight transfer; `usbd_edpt_xfer()` is harmless if already queued
- `ecm_report(true)`: sends the `NETWORK_CONNECTION` (Connected) notification;
  the notification completion interrupt will then call `ecm_report(false)` (speed change)
  via `netd_xfer_cb:364-366` automatically

#### 1.2 Declaration in `net_device.h`

**File**: `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/net_device.h`
**Location**: Application API section, after `tud_network_xmit()` (after line 69)

```c
// Re-announce ECM link to host after USB bus-level resume (ECM mode only).
// Call from tud_resume_cb() to send NETWORK_CONNECTION when host does not
// re-send SET_INTERFACE alt=1 across a suspend/resume cycle.
void tud_network_ecm_resume(void);
```

### Success Criteria

#### Automated Verification:
- [x] Firmware builds without errors: `cmake --build build`

---

## Phase 2: Update `tud_resume_cb()` in `net_glue.c`

### Overview

Call the new `tud_network_ecm_resume()` function and restart mDNS from `tud_resume_cb()`
so the host gets `NETWORK_CONNECTION` and mDNS re-probes `pyro.local` immediately on wake.

### Changes Required

#### 2.1 Update `tud_resume_cb`

**File**: `src/net_glue.c`
**Location**: `tud_resume_cb()` at line 164

```c
/* Before */
void tud_resume_cb(void) {
    lwip_uart_printf("!USB resume\r\n");
    netif_set_link_up(&netif_data);
}

/* After */
void tud_resume_cb(void) {
    lwip_uart_printf("!USB resume\r\n");
    netif_set_link_up(&netif_data);
    tud_network_ecm_resume();   /* re-announce NETWORK_CONNECTION to macOS */
    if (mdns_started) {
        mdns_resp_restart(&netif_data);
    }
}
```

**Note on mDNS**: `tud_network_init_cb()` already calls `mdns_resp_restart()` for the
`SET_INTERFACE` cycling path. Adding it here covers the bus suspend/resume path where
`tud_network_init_cb()` is not called. The two paths are mutually exclusive for a given
event, so there is no double-restart risk.

**Note on double `ecm_report(true)`**: If macOS does re-send `SET_ETHERNET_PACKET_FILTER`
after receiving `NETWORK_CONNECTION` (which it may do on some OS versions), `ecm_report(true)`
would be called a second time via the filter handler. `netd_report()` uses
`usbd_edpt_claim()` which serializes endpoint access — the second call will simply not
fire if ep_notif is busy with the first notification. This is harmless.

### Success Criteria

#### Automated Verification:
- [x] Firmware builds without errors: `cmake --build build`

#### Manual Verification:
- [ ] Start `ping 192.168.7.1` from Mac. Let it run. Put Mac to sleep (Apple menu →
  Sleep). Wake Mac after ~10 seconds. Verify ping recovers without hardware reset.
- [ ] UART log shows: `!USB suspend` on sleep, `!USB resume` on wake, `$PYRO` telemetry
  resumes immediately.
- [ ] `ping pyro.local` also recovers after wake (mDNS re-probe).
- [ ] Web interface at `http://pyro.local` loads fully after wake.
- [ ] Repeat sleep/wake 3 times to confirm consistent recovery.

**Implementation Note**: After all manual verification passes, this plan is complete.

---

## Implementation Notes (Post-Coding)

### Revision from initial implementation

The first attempt called `tud_network_ecm_resume()` directly from `tud_resume_cb()`. This
failed silently: `tud_resume_cb` fires from within `tud_task()` while the USB bus is still
in the process of resuming. At that moment, `usbd_edpt_claim()` (inside `netd_report()`)
returns false because the notification endpoint is still marked busy from pre-suspend state.
The NETWORK_CONNECTION notification is never transmitted.

Additionally, `tud_network_recv_renew()` was removed from `tud_network_ecm_resume()`.
The RP2040 USB hardware preserves pending endpoint transfers across suspend/resume; calling
`usbd_edpt_xfer()` on an already-active OUT endpoint is unsafe.

### Final implementation

**Deferred flag pattern**: `tud_resume_cb()` sets `ecm_resume_pending = true` and returns.
`net_service()` checks the flag on the very next iteration (after `tud_task()` returns in
`hal_platform_service()`), calls `tud_network_ecm_resume()` + `mdns_resp_restart()`, then
clears the flag. At this point the USB bus is fully active and `usbd_edpt_claim()` succeeds.

Call sequence per `hal_platform_service()` iteration:
```
tud_task()     → tud_resume_cb fires → ecm_resume_pending = true
net_service()  → flag set → tud_network_ecm_resume() + mdns_resp_restart()
                           → ecm_report(true) → NETWORK_CONNECTION sent to macOS
```

`tud_network_ecm_resume()` (`ecm_rndis_device.c`): guards on `ecm_mode` + `ep_in != 0`,
resets `can_xmit = true`, calls `ecm_report(true)`.

---

## Testing Strategy

### Manual Testing Steps

1. Flash firmware. Connect USB. Verify `ping 192.168.7.1` succeeds.
2. Start a continuous ping: `ping 192.168.7.1`
3. Put Mac to sleep (Apple menu → Sleep or close lid).
4. Wait ~10 seconds. Wake Mac.
5. Observe UART log: should see `!USB suspend` then `!USB resume`.
6. Observe ping output: should recover within ~5 seconds of wake.
7. Verify `ping pyro.local` also resolves.
8. Load `http://pyro.local` — full web interface should load.
9. Repeat steps 3–8 two more times.

### Edge Cases

- **First boot (ep_in == 0)**: `tud_network_ecm_resume()` returns early before `SET_INTERFACE
  alt=1` has ever been received. `tud_resume_cb()` still calls `netif_set_link_up()`, which
  is the correct behavior.
- **RNDIS mode**: `tud_network_ecm_resume()` returns early (`!ecm_mode`). No change in
  behavior for RNDIS-mode hosts.
- **Double notification**: `usbd_edpt_claim()` inside `netd_report()` serializes correctly.

---

## Code References

- `src/net_glue.c:164-168` — `tud_resume_cb()` (to update)
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:198-221` — `ecm_report()` static function
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:83` — `static bool can_xmit`
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:251-264` — existing `SET_INTERFACE alt=1` patch (context)
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/net_device.h:60-70` — Application API section (add declaration)
- `src/net_glue.c:35` — `mdns_started` static bool
- Research: `thoughts/shared/research/2026-04-05-macos-sleep-wake-uart-trace.md`
- Prior plan: `thoughts/shared/plans/2026-04-04-usb-liveness-mdns-restart.md`
