---
date: 2026-04-04T00:00:00-07:00
researcher: Joseph Julicher
git_commit: 111637357a337d93f1c1f3f3953d3e908eff2717
branch: main
repository: pyro_fw
topic: "Why does the web interface stop responding after approximately 30 minutes"
tags: [research, codebase, web, http, tcp, lwip, usb, networking, timeout]
status: complete
last_updated: 2026-04-04
last_updated_by: Joseph Julicher
last_updated_note: "Added follow-up research: 3390s specific uptime data point, dhserver.c no-timer confirmation, exhaustive timer inventory, no firmware mechanism explains the specific timing"
---

# Research: Why Does the Web Interface Stop Responding After ~30 Minutes

**Date**: 2026-04-04
**Researcher**: Joseph Julicher
**Git Commit**: `111637357a337d93f1c1f3f3953d3e908eff2717`
**Branch**: main
**Repository**: pyro_fw

## Research Question

Why does the web interface stop responding after approximately 30 minutes?

## Summary

**There are no 30-minute (1800-second) timeout constants anywhere in the firmware or web frontend.** The ~30-minute failure window is not caused by any explicit timer in the codebase. The most likely explanations are host-OS-level behaviors acting on the USB network adapter (CDC-ECM/RNDIS), not anything the firmware controls.

The browser frontend polls `/api/status` every 1 second indefinitely with no stop mechanism. The HTTP server uses `Connection: close` on every response, so each poll creates and destroys a TCP connection. The server has no idle-connection timeouts. The failure most likely originates outside the firmware: USB selective suspend on Windows, ARP/NDP cache expiry on macOS/Linux, or Windows RNDIS driver housekeeping — all of which fall in the 20–30-minute range by OS default.

There are three structural firmware conditions that could also contribute, none of which have 30-minute timers but which could accumulate over time:
1. TCP PCB pool (`MEMP_NUM_TCP_PCB = 16`) exhaustion if TIME_WAIT sockets pile up
2. pbuf pool (`PBUF_POOL_SIZE = 24`) exhaustion if RX frames arrive faster than the main loop drains them
3. HTTP connection pool (`conn_pool[8]`) exhaustion under concurrent access

---

## Detailed Findings

### 1. Web Frontend Polling Model (`www/app.js`)

The entire client communication model is HTTP short-polling — there are no WebSockets or persistent connections.

- **`update()` function** (`app.js:43–118`): Issues `fetch('/api/status')` then reschedules itself with `setTimeout(update, 1000)` at `app.js:117`. This runs unconditionally whether the fetch succeeded or failed — the loop never stops.
- **Connection loss indicator**: `missCount` is incremented on each failed fetch (`app.js:114`). When `missCount > 3`, the string `'Connection lost'` is written to the `sState` cell (`app.js:115`). No other UI change occurs and no recovery action is taken beyond continuing to poll.
- **No stop mechanism**: The `setTimeout` chain in `update()` cannot be cancelled once started. It runs for the lifetime of the browser tab.

All timer values in the frontend:

| Location | Value | Purpose |
|---|---|---|
| `app.js:117` | 1000 ms | Main poll interval (infinite) |
| `app.js:422` | 2000 ms | Reboot-recovery poll interval |
| `app.js:411` | 30 attempts × 2000 ms = 60 s | Reboot-recovery timeout window |

**No 30-minute value exists anywhere in `app.js`.**

---

### 2. HTTP Server: Connection Lifecycle (`src/http_server.c`)

The server is a bare-metal callback-driven HTTP/1.1 server on top of lwIP's raw TCP API (`NO_SYS = 1`). Every response carries `Connection: close`. Connections have no idle timeout.

- **Every HTTP exchange is a new TCP connection**: `on_accept()` → `on_recv()` → response → `on_sent()` → `tcp_close()`. There is no persistent connection or keep-alive.
- **No `tcp_poll` callback is ever registered**: There is no application-level idle timeout anywhere. A connection that never sends or receives data after being accepted will sit open indefinitely.
- **Connection pool**: `conn_pool[8]` at `http_server.c:41` — 8 simultaneous `conn_state_t` slots. If all 8 are in use when a new connection arrives, the new connection is dropped immediately via `tcp_close()`.
- **lwIP TCP PCB limit**: `MEMP_NUM_TCP_PCB = 16` (`lwipopts.h`). TCP connections in TIME_WAIT state also consume PCB slots. With a 1-second poll interval and lwIP's default TIME_WAIT of ~120 seconds, up to ~120 PCBs could be needed — but only 16 are available. lwIP recycles TIME_WAIT PCBs when the pool is exhausted, so this is self-correcting in practice.

---

### 3. lwIP and Network Transport (`src/lwipopts.h`, `src/net_glue.c`)

The network transport is **USB CDC-ECM/RNDIS** (TinyUSB → lwIP). The firmware is the network; the host OS treats the device as a USB network adapter.

Key lwIP limits:

| Parameter | Value | Location |
|---|---|---|
| `MEMP_NUM_TCP_PCB` | 16 | `lwipopts.h` — max simultaneous TCP PCBs |
| `PBUF_POOL_SIZE` | 24 | `lwipopts.h` — RX packet buffer pool |
| `MEM_SIZE` | 8000 bytes | `lwipopts.h` — lwIP heap |
| `TCP_SND_BUF` | ~5840 bytes | `lwipopts.h` — per-connection TX buffer |

**Keepalive**: `LWIP_TCP_KEEPALIVE` is not defined in `lwipopts.h` — TCP keepalives are disabled. The lwIP default applies (keepalives off).

**pbuf exhaustion path** (`net_glue.c:102–117`): If `pbuf_alloc()` returns NULL, the incoming USB frame is dropped and `net_rx_drop` is incremented. The firmware reports this counter via `/api/status`. If the pool is persistently exhausted, all inbound HTTP requests are silently discarded.

**RX frame single-slot bottleneck** (`net_glue.c:103–105`): Only one `received_frame` can be pending at a time. If the main loop doesn't call `net_service()` fast enough to drain frames between USB interrupts, frames are dropped and `net_rx_drop` increments.

---

### 4. Main Loop Structure (`src/main_hardware.c:61–83`)

The main loop runs as fast as the CPU allows with no sleep:

```
while (1) {
    hal_platform_service()      // tud_task() + net_service() + mdns_poll()
    check pending_reset
    hal_tasks_tick(now)         // pressure, buzzer, log flush tasks
    dispatch_state(&ctx, now)   // flight FSM
    flight_update_outputs()
    update_status()
}
```

`hal_sleep_until_event()` is a no-op (`hal_hardware.c:568–570`), explicitly disabled in v2.1.27 to keep USB polling continuous.

**Blocking calls that can stall the loop** (and therefore stall lwIP):
- `pyro_check_continuity()` → `sleep_ms(10)` (`pyro.c:58`) — at most every 1000 ms
- `ota_flush()` → `flash_range_erase()` + `flash_range_program()` with interrupts disabled — only during OTA
- Log buffer overflow path: `hal_fs_write()` synchronous flash write (`hal_hardware.c:744`)

None of these approach 30 minutes.

---

### 5. Complete Timeout Constant Inventory

There is no 30-minute or 1800-second constant anywhere in the codebase.

| Constant | Value | Location | Meaning |
|---|---|---|---|
| Landing timeout (configurable) | 60 s default | `config_fields.h:41` | Force LANDED state after 60 s of descent |
| Backup apogee timer (configurable) | 30 s default | `config_fields.h:40` | Force apogee if no vertical speed reversal in 30 s |
| DHCP lease duration | 86400 s (24 h) | `net_glue.c:41–42` | Two clients at 192.168.7.2 and 192.168.7.3 |
| Ground test arm timeout | 3000 ms | `ground_test.h:55` | Auto-disarm 3 s after arm with no fire |
| Boot settle | 2500 ms | `flight_states.c:174` | Wait before continuity check |
| Reboot recovery timeout | 60 s | `app.js:411` | Browser-side: 30 attempts × 2 s |
| Main poll interval | 1000 ms | `app.js:117` | Browser-side HTTP poll |

---

## Architecture Documentation

### How the Web Stack Works End-to-End

```
Browser (app.js)
  └── fetch('/api/status') every 1000 ms
        │
        ▼ USB (CDC-ECM or RNDIS)
TinyUSB → tud_network_recv_cb() → pbuf_alloc → received_frame
        │
        ▼ net_service() (called each main loop iteration)
ethernet_input() → lwIP TCP stack → on_recv() callback
        │
        ▼ http_server.c
conn_alloc() from conn_pool[8]
Route request → build JSON response
tcp_write() → on_sent() → tcp_close()
```

### Why ~30 Minutes Is Likely Host-OS Behavior

The device presents to the host as a USB network adapter. Several host-OS mechanisms activate in the 20–30-minute range:

1. **Windows USB Selective Suspend**: Windows can suspend USB devices that appear idle. For RNDIS class devices, the default suspend timeout varies by driver version but commonly falls in the 20–30-minute range. Once suspended, the device's network interface becomes unreachable until USB activity wakes it.

2. **ARP/NDP Cache Expiry**: macOS and Linux maintain ARP caches with a default lifetime of roughly 20 minutes (macOS: `net.link.ether.inet.arp_expire = 1200` seconds = 20 minutes). Once the ARP entry for the device expires, the OS needs to re-ARP before it can send packets. If the ARP reply is missed (e.g., due to a pbuf pool exhaustion event or a USB scheduling gap), the connection appears dead until the ARP cache is refreshed by a successful exchange.

3. **Windows RNDIS Driver State**: Windows RNDIS drivers can silently reset the virtual NIC after a period of inactivity or on internal driver housekeeping cycles. This can cause the IP address assignment or route to be lost without any explicit notification to the browser.

4. **Browser Timer Throttling**: When the browser tab is backgrounded (not visible), Chrome and Firefox throttle `setTimeout` timers to fire no more than once per minute. With a 1-second poll that becomes a 60-second poll, the browser's visible state will lag and will show "Connection lost" after 4 missed polls (4 × 60 s = 4 minutes in the background — shorter than 30 minutes, so this is not the primary explanation unless the tab has been backgrounded for an extended period).

### Diagnostic Data Available

The firmware exposes counters in `/api/status` (`g_status` in `main_hardware.c`) that can confirm or rule out firmware-side resource exhaustion:
- `net_rx_drop` — pbuf pool or single-slot RX drops
- `net_tx_fail` — TX link busy failures
- `net_http_accept` — total connections accepted (from `main_hardware.c:23`)
- `net_conn_full` — connection pool exhaustion events (declared at `main_hardware.c:25`, visible in status JSON)

---

## Code References

- `www/app.js:43–118` — `update()` polling loop
- `www/app.js:114–117` — connection failure handling and loop rescheduling
- `www/app.js:408–423` — `waitForReboot()` separate recovery loop (60-second window)
- `src/http_server.c:41` — `conn_pool[8]` static connection pool
- `src/http_server.c:43–49` — `conn_alloc()` linear scan, returns NULL when full
- `src/http_server.c:596–604` — `on_accept()`, no `tcp_poll` registered
- `src/lwipopts.h` — all lwIP pool sizes (PCB=16, pbuf=24, heap=8000)
- `src/net_glue.c:102–117` — `tud_network_recv_cb()`, pbuf alloc and drop logic
- `src/net_glue.c:103–105` — single-slot received_frame bottleneck
- `src/net_glue.c:61–77` — `linkoutput_fn()` TX with 20-retry bounded spin
- `src/net_glue.c:177–185` — `net_service()`, drives all lwIP callbacks
- `src/main_hardware.c:61–83` — main loop structure
- `src/hal_hardware.c:568–570` — `hal_sleep_until_event()` no-op
- `src/config_fields.h:40–41` — 30 s backup apogee timer, 60 s landing timeout (the only "30" in the codebase)

## Open Questions

- What host OS and USB class (CDC-ECM vs RNDIS) are in use when the 30-minute failure occurs? This would narrow down whether it's Windows RNDIS housekeeping, macOS ARP expiry, or something else.
- Does the connection fully fail (browser reports network error) or does the browser continue to show the last-known status while polling silently fails? The former points to a transport-layer (USB/IP) disruption; the latter points to firmware resource exhaustion where lwIP is still alive but dropping requests.
- What does `/api/status` show for `net_rx_drop` and `net_conn_full` at the time of failure? Non-zero values would confirm firmware-side resource pressure.
- Does cycling the browser tab (close/reopen to same URL) restore connectivity, or is a USB re-plug required? USB re-plug required = selective suspend or RNDIS reset; browser refresh works = TCP state stuck at OS layer.

---

## Follow-up Research 2026-04-04

**New diagnostic observations from live session:**
- `pyro.local` (mDNS) — not responding at time of failure
- `192.168.7.1` (direct IP) — not responding at time of failure
- `ping 192.168.7.1` — **no response** (ICMP-level failure, below HTTP/TCP)
- UART telemetry — absent (consistent with ground/pre-flight state where telemetry is not emitted)
- Reset button press — brief partial recovery: single ping response + partial web page load
- USB replug — **full recovery**, web page functional, uptime counter confirmed alive at 41 s

### What These Observations Establish

**The failure is at the USB transport layer, not at the HTTP or TCP layer.**

Ping failing means no IP connectivity whatsoever — ARP is not working, or Ethernet frames are not reaching lwIP at all. This eliminates every firmware-side HTTP/TCP explanation (connection pool exhaustion, pbuf starvation, TCP TIME_WAIT). Those conditions would cause HTTP requests to fail while ICMP (ping) would still succeed. Ping failure means the Ethernet frame never arrives at `ethernet_input()`.

**The firmware is still running at the time of failure.**

USB replug restores a 41-second uptime counter — the firmware did not crash or freeze. The cooperative main loop (`main_hardware.c:61–83`) was spinning the entire time. UART silence is expected on-ground behavior (telemetry is gated by flight state in `flight_states.c:622`).

**Reset button vs replug behavior reveals host-side USB driver state divergence.**

A firmware reset causes the RP2040 USB hardware to re-enumerate on the bus. The host detects a disconnect/reconnect for the same VID `0x2E8A` / PID `0x4002`. On macOS with CDC-ECM, the host driver may re-use its cached adapter state rather than fully reinitializing — leading to partial restoration (one ping, partial page). A physical USB replug forces the host to fully tear down the adapter driver instance and re-initialize from scratch, which is why it reliably restores full function.

### TinyUSB and USB Network Stack Details

**USB class in use:** `CFG_TUD_ECM_RNDIS = 1` (`tusb_config.h:50`) — dual-configuration device. Windows selects RNDIS (Configuration 0); macOS/Linux selects CDC-ECM (Configuration 1). Both use the same endpoint numbers (`0x81` notify, `0x02` bulk-out, `0x82` bulk-in).

**No USB disconnect/reconnect callbacks are implemented.** `tud_umount_cb()`, `tud_suspend_cb()`, `tud_resume_cb()` are not overridden anywhere — TinyUSB's default no-op weak implementations apply (`src/` has no implementations of these).

**`tud_network_init_cb()` is the only USB reconnect hook** (`net_glue.c:126–132`). It clears `received_frame` (freeing any in-flight pbuf) and emits a UART log message. Nothing else is re-initialized.

**`NETIF_FLAG_LINK_UP` and `NETIF_FLAG_UP` are set unconditionally at `netif_init_cb()` (`net_glue.c:85`) and never cleared.** lwIP always believes the link is active. There is no `netif_set_link_down()` call anywhere in the firmware. lwIP's mDNS, ARP, and TCP subsystems all operate as if the link is continuously up.

**`mdns_started` is a static bool (`net_glue.c:155`) set on the first `net_mdns_poll()` call and never reset.** If the USB host disconnects and reconnects without a firmware reboot, `net_mdns_poll()` returns immediately on every call. The lwIP mDNS module remains registered on the original `netif_data` from initialization — it continues to announce over USB once USB connectivity resumes, since `netif_data` itself is persistent.

### Why Reset Gives Only Partial Recovery

On firmware reset (reset button):
1. RP2040 restarts, TinyUSB calls `tud_init()` at `hal_hardware.c:582`
2. USB hardware re-enumerates — host detects disconnect/reconnect
3. On macOS with CDC-ECM: the host network driver may reuse its existing interface instance (same VID/PID/config), retaining stale ARP cache entries and TCP state
4. The firmware's LittleFS serves `index.html` to the first successful HTTP request (the "partial page") — but `app.js` is a second GET request which may fail if the host-side ARP or TCP state becomes inconsistent before the second request completes

### Why USB Replug Gives Full Recovery

On USB replug:
1. Host OS fully tears down the CDC-ECM or RNDIS adapter driver instance
2. On replug, a fresh driver instance is created with empty ARP cache, no stale TCP connections, fresh DHCP lease request
3. The device issues a new DHCP offer from `dhserver.c` (the lease table has 2 slots at `192.168.7.2` and `192.168.7.3` with 24-hour leases — `net_glue.c:40–43`)
4. `tud_network_init_cb()` fires — clears `received_frame`
5. lwIP state on the device side persists (it was never reset), but since the host initiates all connections, stale device-side TCP PCBs simply time out naturally

### The `received_frame` Single-Slot Stall (Self-Healing)

The `received_frame` single-slot (`net_glue.c:20`) is not a persistent stall. If the slot is occupied when a new frame arrives:
- `tud_network_recv_cb` returns `false` → TinyUSB back-pressures the OUT endpoint
- Within one main loop iteration, `net_service()` consumes the existing frame and calls `tud_network_recv_renew()`
- The endpoint is re-armed and the next frame can arrive

The main loop runs at CPU speed with no sleep, so the slot is cleared within microseconds. This mechanism cannot cause a 30-minute outage.

### Updated Code References (USB Layer)

- `src/tusb_config.h:50` — `CFG_TUD_ECM_RNDIS = 1`, dual RNDIS+ECM mode
- `src/usb_descriptors.c:39–54` — VID `0x2E8A`, PID `0x4002`, `bNumConfigurations = 2`
- `src/usb_descriptors.c:61–74` — RNDIS (Config 0) and ECM (Config 1) descriptors
- `src/net_glue.c:20` — `static struct pbuf *received_frame` single-slot
- `src/net_glue.c:32` — hardcoded MAC `02:02:84:00:6A:00`
- `src/net_glue.c:40–43` — DHCP lease table: 2 clients, 86400 s leases
- `src/net_glue.c:61–77` — `linkoutput_fn()` — checks `tud_ready()`, 20-retry spin, returns `ERR_USE` when USB not ready
- `src/net_glue.c:83–92` — `netif_init_cb()` — sets `NETIF_FLAG_LINK_UP` unconditionally, never cleared
- `src/net_glue.c:102–118` — `tud_network_recv_cb()` — single-slot deposit, drop on full slot
- `src/net_glue.c:126–132` — `tud_network_init_cb()` — only hook for USB reconnect, clears `received_frame`
- `src/net_glue.c:146–153` — `mdns_name_result()` — conflict handler increments `mdns_suffix`
- `src/net_glue.c:155` — `static bool mdns_started` — one-shot init, never reset
- `src/net_glue.c:165–175` — `net_mdns_poll()` — returns immediately after first call
- `src/net_glue.c:177–185` — `net_service()` — single-slot drain + `sys_check_timeouts()`
- `src/hal_hardware.c:582` — `tud_init(BOARD_TUD_RHPORT)` — called once at startup
- `src/hal_hardware.c:604` — `tud_task()` — called every main loop iteration

---

## Follow-up Research 2026-04-04 — 3390-Second Specific Uptime Observation

**New observation**: Network stopped responding after an uptime of exactly **3390 seconds** (56.5 minutes). GP25 LED on at normal brightness, confirming the main loop was running at full speed (GP25 is XOR-toggled every iteration with no rate-limiting at `main_hardware.c:63`). Symptoms are consistent with prior follow-up (USB transport layer failure): ping presumably not responding, web interface dead, main loop alive.

### Comprehensive Timer Inventory at 3390 Seconds

An exhaustive audit of every timer, counter, and time-dependent mechanism in the firmware confirms: **there is no value in the firmware that reaches any threshold at or near 3390 seconds.**

All periodic and one-shot timer values, with their values converted to seconds:

| Mechanism | Period / Expiry | Type | Source |
|---|---|---|---|
| `to_ms_since_boot()` rollover | 4,294,967 s (~49.7 days) | `uint32_t` ms | `hal_hardware.c:224` |
| DHCP lease time | 86,400 s (24 h) | config field | `net_glue.c:41–42` |
| ARP_MAXAGE (lwIP default) | 300 s — repeating | cycles | `lwipopts.h` (not overridden); confirmed by `etharp_tmr` disassembly: `movs r1, #150; lsls r1, r1, #1` = 300 |
| mDNS 25%-TTL re-announce | ~29.5 s — repeating | cyclic | `mdns_multicast_timeout_25ttl_reset_ipv4`; word `0x00007530` = 29,488 ms at `build_test/pyro_fw_c.dis:31340` |
| mDNS record TTL | ~118 s per record | per-announcement | implied from 25%-TTL = 29,488 ms → full TTL ≈ 117,952 ms |
| mDNS restart probe delay | 5,000 ms | one-shot | word `0x00001388` at `build_test/pyro_fw_c.dis:28536`; only fires after `mdns_resp_restart()`, which is **never called** |
| TCP TIME_WAIT | ~20 s | per-PCB | 40 × 500 ms; `cmp r3, #40` at `build_test/pyro_fw_c.dis:14455` |
| TCP FIN_WAIT_2 close | ~120 s | per-PCB | 240 × 500 ms; `cmp r2, #240` at `build_test/pyro_fw_c.dis:14519` |
| `etharp_tmr` fire | 1,000 ms — repeating | cyclic | lwIP default `ETHARP_TMR_INTERVAL` |
| `tcp_tmr` fast | 250 ms — repeating | cyclic | lwIP default |
| `tcp_tmr` slow | 500 ms — repeating | cyclic | lwIP default |
| `igmp_tmr` | 100 ms — repeating | cyclic | lwIP default |
| Pressure sample | 20 ms — repeating | task | `hal_hardware.c:258–261` |
| Log flush | 200 ms — repeating | task | `hal_hardware.c:651` (flight only) |
| `telemetry_seq` `uint16_t` rollover | 65,535 s at 1 Hz (PAD_IDLE) | `uint16_t` | `flight_states.h:108`; sequence number only, no network effect |
| Boot settle | 2,500 ms — one-shot | `uint32_t` | `flight_states.c:174` |
| Backup apogee timer | 30 s — one-shot (flight only) | `uint8_t` config × `uint32_t` | `config_fields.h:40` |
| Landing timeout | 60 s — one-shot (flight only) | `uint8_t` config × `uint32_t` | `config_fields.h:41` |

**3390 seconds does not appear in or near any of these values.**

### dhserver.c Has No Internal Timer

The DHCP server implementation from TinyUSB's `lib/networking/dhserver.c` (linked at `CMakeLists.txt` line referencing `${TINYUSB_DIR}/lib/networking/dhserver.c`) is a purely reactive UDP responder:

- It binds to UDP port 67 via `udp_bind()` and installs `udp_recv_proc()` as its receive callback
- `udp_recv_proc()` handles only `DHCP_DISCOVER` and `DHCP_REQUEST` packets
- There is **no periodic timer**, no lease expiry tracking, no renewal check, no internal state machine
- The 86,400-second lease time is encoded only in the `DHCP_LEASETIME` option in outbound OFFER/ACK packets — the server never checks it again
- Lease entries are stored in the `dhcp_entry_t entries[]` array in `net_glue.c:40–43` and are cleared only when a new client sends a `DHCP_REQUEST` and calls `free_entry()` first

**Implication**: The dhserver cannot handle DHCP renewal requests. RFC 2131 renewal (DHCPREQUEST in RENEWING state) is sent unicast without option 50 (`DHCP_IPADDRESS`). The handler does `find_dhcp_option(..., DHCP_IPADDRESS)` and breaks immediately on NULL return — no ACK is sent. However, T1 for a 86,400-second lease = 43,200 seconds (12 hours), which is far beyond 3390 seconds and does not explain this observation.

### Why 3390 Seconds Specifically

The 3390-second timing does not correspond to any firmware constant. The most structurally sound explanation remains the USB transport layer conclusion from prior research: the host OS's RNDIS or CDC-ECM driver entered a non-functional state. The 3390-second (56.5-minute) timing is consistent with:

- **Windows RNDIS power management**: Windows can reset virtual NIC state on internal housekeeping cycles that vary by driver version and power plan. The timing is not fixed and can range from 20 minutes to over an hour.
- **Cumulative ARP stress**: lwIP's `etharp_tmr` fires every 1 second and ages entries by 1 tick. At `ARP_MAXAGE = 300` (5-minute cycles), by 3390 seconds the ARP entry for the host has expired and been re-ARPed ~11 times. If any single ARP refresh coincides with a transient USB scheduling gap on the host, the ARP reply is missed, lwIP queues outbound packets waiting for ARP resolution, the TCP polling loop stops receiving responses, and the host-side TCP stack eventually gives up on the connection.
- **Non-reproducible USB event**: Physical USB intermittency (cable, controller power state) at the specific 3390-second timestamp.

The observation is a **single data point**. Without a reproducible sequence at the same uptime, the 3390-second value is not diagnostic of a deterministic firmware mechanism. If the failure can be reproduced at the same uptime repeatedly, that would point to an OS-level timer; if the failure time varies across runs, it confirms the non-deterministic USB transport failure mode.

### Diagnostic Suggestion for Confirming USB Layer

To distinguish deterministic firmware failure from non-deterministic USB failure at the next occurrence:

1. At time of failure, check `net_rx_drop`, `net_tx_fail`, `net_conn_full` from UART or if available via serial console — non-zero = resource exhaustion; all zero = USB layer
2. `ping 192.168.7.1` — failure at ICMP level confirms USB/Ethernet layer, not HTTP/TCP
3. Check if failure time is consistent across multiple runs at the same environment; variation rules out a firmware timer

### Additional Code References

- `lib/networking/dhserver.c` (TinyUSB SDK, linked via `CMakeLists.txt`) — `udp_recv_proc()`: handles only DISCOVER and REQUEST, no timer, no renewal handling
- `src/main_hardware.c:63` — `gpio_xor_mask(1u << 25)` — GP25 toggled every iteration, no rate limit; brightness indicates loop speed not a state
- `src/net_glue.c:85` — `NETIF_FLAG_LINK_UP` set unconditionally, never cleared regardless of USB state
- `build_test/pyro_fw_c.dis:22972–22974` — confirmed `ARP_MAXAGE = 300` from `etharp_tmr` disassembly
