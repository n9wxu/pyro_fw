---
date: 2026-04-05T17:36:03-07:00
researcher: Joseph Julicher
git_commit: 442a2f1ceb4af3876d74430a66fd9476d19a0a6f
branch: main
repository: pyro_fw
topic: "Verify AI claim: macOS requires NETWORK_CONNECTION CDC notification for ECM routing after wake"
tags: [research, codebase, usb, ecm, tinyusb, macos, networking, cdc-ecm, set-interface]
status: complete
last_updated: 2026-04-05
last_updated_by: Joseph Julicher
---

# Research: macOS CDC-ECM Wake/Reconnect — Does NETWORK_CONNECTION Matter?

**Date**: 2026-04-05  
**Researcher**: Joseph Julicher  
**Git Commit**: `442a2f1ceb4af3876d74430a66fd9476d19a0a6f`  
**Branch**: main  
**Repository**: pyro_fw

## Research Question

An AI analysis proposed the following failure mechanism and fix:

> "macOS puts the network software stack to sleep without sending any USB-level messages — the USB bus stays fully active (no suspend, no disconnect). From macOS's side the IOUSBNetworkingFamily driver stops routing packets and waits for a fresh NETWORK_CONNECTION CDC notification to bring routing back. The device never sends one because it only does so in response to SET_ETHERNET_PACKET_FILTER, which macOS doesn't re-send after wake. The fix requires the device to proactively re-send the ECM NETWORK_CONNECTION notification … implement an idle watchdog."

Is this diagnosis accurate? Is the proposed watchdog fix correct or unnecessary?

---

## Summary

**The AI's diagnosis is incorrect.** The actual failure mechanism is well-documented in the codebase itself, has already been diagnosed in prior research, and has already been fixed.

**What actually happens:** macOS sends `SET_INTERFACE alt=0` (deactivate ECM data interface) and then `SET_INTERFACE alt=1` (reactivate) when cycling the network interface. The original upstream TinyUSB code had a bug where the second and subsequent `SET_INTERFACE alt=1` calls were silently ignored (because endpoint open was guarded by `ep_in == 0 && ep_out == 0`). This left `can_xmit = false` and the OUT endpoint un-armed after the first sleep/wake cycle. The fix was to move `tud_network_init_cb()`, `can_xmit = true`, and `tud_network_recv_renew()` outside that guard.

**The AI's proposed watchdog fix is not only unnecessary, it addresses the wrong problem.** macOS does not require a NETWORK_CONNECTION notification to resume routing — it uses `SET_INTERFACE alt=1` to reactivate the data interface, and data transfer resumes normally once the device correctly re-arms its endpoints.

---

## Detailed Findings

### 1. The TinyUSB ECM Driver: Upstream vs. In-Use

The build uses pico-sdk 2.2.0 (`PICO_SDK_PATH: /Users/joejulicher/.pico-sdk/sdk/2.2.0`), whose TinyUSB includes the ECM/RNDIS class driver at:

```
/Users/joejulicher/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c
```

This file has a **local modification** (not upstream). The diff reveals:

**Upstream (original) behavior:**
```c
if (_netd_itf.ep_in == 0 && _netd_itf.ep_out == 0) {
    // Only on first activation
    TU_ASSERT(usbd_open_edpt_pair(...));
    tud_network_init_cb();
    can_xmit = true;
    tud_network_recv_renew();
}
// On second activation (re-activation), NOTHING HAPPENS
```

**Patched behavior (in-use):**
```c
if (_netd_itf.ep_in == 0 && _netd_itf.ep_out == 0) {
    // Open endpoints ONLY on first activation
    TU_ASSERT(usbd_open_edpt_pair(...));
}
// Always reinitialize on activation (first or re-activation after host sleep/wake).
// Without this, can_xmit stays false and the OUT endpoint has no queued transfer,
// so the device cannot send or receive data after macOS cycles SET_INTERFACE alt=0/1.
tud_network_init_cb();
can_xmit = true;
tud_network_recv_renew();
```

The patch comment explicitly names the failure mode: **macOS cycles SET_INTERFACE alt=0/1**, and the upstream TinyUSB silently dropped the second `alt=1`. The patch is already in place.

### 2. What macOS Actually Sends: USB-Level Messages ARE Sent

The AI claims "macOS puts the network software stack to sleep without sending any USB-level messages." The codebase evidence directly contradicts this:

- The patch comment explicitly documents that macOS **does** send `SET_INTERFACE alt=0` and `SET_INTERFACE alt=1`
- These are standard USB control transfers — they ARE USB-level messages
- The `tud_suspend_cb` / `tud_resume_cb` callbacks are also implemented (`net_glue.c:157–168`), suggesting USB bus-level suspend/resume can also occur

macOS appears to use both mechanisms depending on context:
- For interface deactivation without full system sleep: `SET_INTERFACE alt=0/1`
- For system sleep/wake: USB bus suspend/resume (triggering `tud_suspend_cb` / `tud_resume_cb`)

### 3. NETWORK_CONNECTION Notification: When It's Sent

In `ecm_rndis_device.c`, `ecm_report(true)` (which sends `NETWORK_CONNECTION` bRequest=0x00, wValue=1 "Connected") is called from exactly one place:

```c
// ecm_rndis_device.c:284-289
if (_netd_itf.ecm_mode) {
    if (0x43 /* SET_ETHERNET_PACKET_FILTER */ == request->bRequest) {
        tud_control_xfer(rhport, request, NULL, 0);
        ecm_report(true);  // <-- only place NETWORK_CONNECTION is sent
    }
}
```

There is no call to `ecm_report()` from within the `SET_INTERFACE alt=1` handler. The sequence is:

1. `SET_INTERFACE alt=1` → endpoints armed, `tud_network_init_cb()` called
2. `SET_ETHERNET_PACKET_FILTER` → `NETWORK_CONNECTION` notification sent → `CONNECTION_SPEED_CHANGE` sent

The claim that "The device never sends [NETWORK_CONNECTION] because it only does so in response to SET_ETHERNET_PACKET_FILTER" is technically true for the device side. However, the question the AI never asks is: **does macOS actually need NETWORK_CONNECTION to route packets?**

### 4. Does macOS Re-Send SET_ETHERNET_PACKET_FILTER After SET_INTERFACE alt=1?

The standard ECM initialization sequence (per USB CDC-ECM spec) is:
1. `SET_INTERFACE alt=1` (activate data interface)
2. `SET_ETHERNET_PACKET_FILTER` (configure packet filter → triggers NETWORK_CONNECTION)
3. Data transfer begins

macOS follows this sequence on initial enumeration. The question is whether it re-sends `SET_ETHERNET_PACKET_FILTER` after `SET_INTERFACE alt=1` on re-activation. Based on the evidence:

- The patch fixes the problem by correctly handling `SET_INTERFACE alt=1` re-activation
- If NETWORK_CONNECTION were genuinely required for routing, and macOS didn't send `SET_ETHERNET_PACKET_FILTER` on re-activation, the patch alone would not have been sufficient to diagnose and fix the problem — yet the patch comment confidently identifies it as the fix
- The prior research document (`2026-04-04-web-interface-30min-timeout.md`) identifies USB replug as the recovery mechanism, not a NETWORK_CONNECTION notification

**The most coherent interpretation:** macOS resumes routing purely based on the ECM data interface being active (SET_INTERFACE alt=1 received and endpoints functional). NETWORK_CONNECTION is a courtesy notification from the device to the host, not a gating requirement for host-side packet routing.

### 5. Current State of net_glue.c After All Fixes

The current `net_glue.c` (as of v2.1.41) has all the fixes in place:

| Callback | What it does |
|---|---|
| `tud_network_init_cb()` | Clears `received_frame`, cycles `netif_set_link_down/up`, restarts mDNS if started |
| `tud_mount_cb()` | `netif_set_link_up()` — USB enumeration complete |
| `tud_umount_cb()` | `netif_set_link_down()` — USB unconfigured/disconnected |
| `tud_suspend_cb()` | `netif_set_link_down()` — USB bus suspended |
| `tud_resume_cb()` | `netif_set_link_up()` — USB bus resumed |

`tud_network_init_cb()` is invoked by the patched TinyUSB ECM driver on every `SET_INTERFACE alt=1`, making this the entry point for the full reconnect sequence.

### 6. Evaluation of the Proposed "Idle Watchdog" Fix

The AI proposes:
- Add a public API `net_send_ecm_network_connection()`
- Implement an idle watchdog that calls it when no traffic is seen

**Why this is wrong:**

1. **Wrong diagnosis:** The failure was TinyUSB silently ignoring `SET_INTERFACE alt=1` re-activations. That is already fixed. The watchdog does not address this.

2. **Wrong mechanism:** macOS does not wait for NETWORK_CONNECTION to route packets. Routing resumes after `SET_INTERFACE alt=1` + proper endpoint state.

3. **Sends unsolicited notifications:** CDC-ECM spec section 6.2.4 says NETWORK_CONNECTION should be sent by the device to notify the host of the current link state — but as a notification of truth, not as a watchdog trigger. Sending it speculatively when no `SET_ETHERNET_PACKET_FILTER` was received is not standard and could confuse the host driver.

4. **`can_xmit` is the critical gate, not NETWORK_CONNECTION:** When `SET_INTERFACE alt=1` was silently ignored, `can_xmit` stayed `false` (it was last set to `false` by `do_in_xfer()` and never reset). No amount of NETWORK_CONNECTION notifications would fix that — the endpoint had no pending transfer.

5. **The watchdog might mask other issues:** A periodic unsolicited NETWORK_CONNECTION notification would generate USB traffic that interferes with the natural diagnostic value of idle periods.

---

## Architecture Documentation

### ECM Activation Flow (Current, Patched)

```
macOS cycles interface (sleep or reconnect)
  │
  ├─ SET_INTERFACE alt=0
  │    └─ netd_control_xfer_cb: _netd_itf.itf_data_alt = 0
  │       (endpoints not closed, but noted as inactive)
  │
  └─ SET_INTERFACE alt=1
       └─ netd_control_xfer_cb [PATCHED]:
            if ep_in==0 && ep_out==0: usbd_open_edpt_pair()  // first time only
            tud_network_init_cb()   // ALWAYS on alt=1
              ├─ pbuf_free(received_frame)
              ├─ netif_set_link_down(&netif_data)
              ├─ netif_set_link_up(&netif_data)   // flush ARP cache, IGMP/mDNS re-announce
              └─ mdns_resp_restart(&netif_data)   // re-probe pyro.local
            can_xmit = true          // ALWAYS on alt=1
            tud_network_recv_renew() // ALWAYS on alt=1 (arms OUT endpoint)
  │
  ├─ [macOS may send] SET_ETHERNET_PACKET_FILTER
  │    └─ ecm_report(true): sends NETWORK_CONNECTION (Connected)
  │       ecm_report(false): sends CONNECTION_SPEED_CHANGE (9.728 Mbps)
  │
  └─ Data transfer resumes
```

### Why USB Replug Still Works More Reliably Than SET_INTERFACE Cycling

USB replug forces the host to:
1. Fully tear down the IOUSBNetworkingFamily driver instance
2. Flush all host-side ARP/NDP caches for the interface
3. Issue a fresh DHCP request (the `dhserver.c` responds with a fresh lease)
4. Re-enumerate through `SET_CONFIGURATION` → `SET_INTERFACE alt=1` → `SET_ETHERNET_PACKET_FILTER`

`SET_INTERFACE` cycling may leave stale ARP entries and TCP state on the host side, which can cause partial failures even after the device-side endpoints are correctly re-armed.

---

## Code References

- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:251-264` — **patched** SET_INTERFACE alt=1 handler; always calls `tud_network_init_cb()`, `can_xmit = true`, `tud_network_recv_renew()` on every activation
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:198-221` — `ecm_report()`: sends NETWORK_CONNECTION and CONNECTION_SPEED_CHANGE
- `~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:284-289` — only location where `ecm_report(true)` (NETWORK_CONNECTION) is called: in response to SET_ETHERNET_PACKET_FILTER
- `src/net_glue.c:127-141` — `tud_network_init_cb()`: clears pbuf, cycles link state, restarts mDNS
- `src/net_glue.c:145-168` — four TinyUSB device lifecycle callbacks driving lwIP link state
- `thoughts/shared/research/2026-04-04-web-interface-30min-timeout.md` — prior research identifying USB transport layer failure

## Open Questions

- Does macOS re-send `SET_ETHERNET_PACKET_FILTER` after `SET_INTERFACE alt=1` on every reconnect, or only on initial enumeration? Capturing USB traffic with Wireshark + USB analyzer during a sleep/wake cycle would answer this definitively.
- Are there specific macOS versions where the SET_INTERFACE cycling behavior changed? (macOS 14 Sonoma and 15 Sequoia changed some USB power management behavior.)
- Is the remaining 56-minute (~3390s) failure mode fully explained by the now-patched TinyUSB behavior? Or does stale host-side ARP/TCP state still accumulate?
