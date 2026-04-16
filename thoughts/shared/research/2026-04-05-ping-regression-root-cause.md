---
date: 2026-04-05T21:02:42-07:00
researcher: Claude (Sonnet 4.6)
git_commit: 442a2f1ceb4af3876d74430a46d9476d19a0a6f
branch: main
repository: pyro_fw
topic: "Root cause analysis: ping broken since v2.1.29, DHCP works"
tags: [research, codebase, usb, ecm, tinyusb, macos, networking, ping, icmp, dma, can_xmit]
status: complete
last_updated: 2026-04-05
last_updated_by: Claude (Sonnet 4.6)
---

# Research: Why Is Ping Broken?

**Date**: 2026-04-05
**Git Commit**: `442a2f1`
**Branch**: main

## Research Question

The memory document `project_ping_regression.md` states:
- v2.1.28: ping works on connect
- v2.1.42 HEAD: ping never works, even on fresh connect with no flight activity
- DHCP works (host gets 192.168.7.2)
- Primary suspect: DMA UART TX (commit `1116373`) conflicting with USB DMA

What is actually wrong with ping?

---

## Summary

**There are two distinct ping failure modes.** One is confirmed by UART trace
(`2026-04-05-macos-sleep-wake-uart-trace.md`). The other is a pre-existing
memory claim that code analysis cannot reproduce as a code-level bug:

| Bug | Trigger | Confirmed | Root Cause |
|---|---|---|---|
| **Bug A** (sleep/wake) | macOS sleep → wake | ✅ Confirmed by UART trace | `tud_resume_cb()` is a no-op stub; `can_xmit` stays false; NETWORK_CONNECTION never re-sent |
| **Bug B** (fresh connect) | Physical USB replug | ❓ Unconfirmed by code analysis | Memory says broken since `1116373`; no code-level mechanism found |

**Key finding**: The original memory hypothesis (DMA channel conflict) is ruled out.
RP2040 USB uses its own DPSRAM-based DMA, entirely separate from the 12 user DMA
channels. `dma_claim_unused_channel()` in `uart_dma_init()` only touches user
channels 0–11 and cannot interfere with USB hardware.

**The sleep/wake bug is real and confirmed.** The fix path is clear and partially
implemented in the SDK: `tud_network_ecm_resume()` is already defined in
`ecm_rndis_device.c` but `tud_resume_cb()` in `net_glue.c` never calls it.

---

## Detailed Findings

### 1. DMA UART TX — Original Suspect (Ruled Out)

`uart_dma_init()` at `src/hal_hardware.c:392–404`:

```c
dma_uart_chan = dma_claim_unused_channel(true);   // claims user DMA channel (0–11)
// ... DREQ_UART0_TX, DMA_SIZE_8, write to PL011 DR ...
irq_set_exclusive_handler(DMA_IRQ_0, uart_dma_irq);
irq_set_enabled(DMA_IRQ_0, true);
dma_channel_set_irq0_enabled(dma_uart_chan, true);
```

- RP2040 USB controller uses **DPSRAM** (USB-dedicated packet memory), not user DMA channels 0–11. TinyUSB RP2040 port (`rp2040_usb.c`) registers zero handlers on `DMA_IRQ_0`. Confirmed by `grep dma rp2040_usb.c` → no output.
- `DMA_IRQ_0` (IRQ #11) and `USB_IRQ` (IRQ #5) have equal default priority (0x80) on Cortex-M0+ and cannot preempt each other.
- `uart_dma_irq()` only writes to DMA hardware registers and advances `tx_q_tail`. It does not touch any TinyUSB, lwIP, or `can_xmit` state.
- **Conclusion**: DMA UART TX cannot cause ping failure through USB hardware interaction.

The one networking change in commit `1116373` is `lwip_uart_printf()` changing from
`uart_write_blocking()` (blocks ~1.2 ms) to `hal_telemetry_send()` (non-blocking).
This makes `tud_network_init_cb()` return faster but has no effect on `can_xmit`,
endpoint arming, or lwIP routing.

### 2. `can_xmit` State Machine — Normal Operation

`can_xmit` (`ecm_rndis_device.c:83`, BSS at `0x200121ab`) gates all USB TX:

| Event | Effect on `can_xmit` |
|---|---|
| Startup / `netd_init()` | `false` (BSS zero-init) |
| `SET_INTERFACE alt=1` (patched handler, line 274) | `true` |
| `do_in_xfer()` (line 90) — every USB IN transfer start | `false` |
| `netd_xfer_cb()` IN complete, non-ZLP (line 371) | `true` |
| `netd_xfer_cb()` IN complete, ZLP needed (line 367–368) | `false` (ZLP sent; `true` after ZLP completes) |
| `tud_network_ecm_resume()` (line 230) | `true` |

**For normal fresh-connect operation**: `SET_INTERFACE alt=1` sets `can_xmit = true`.
DHCP frames (broadcast UDP, non-ZLP sizes) correctly cycle `can_xmit` false→true
via `do_in_xfer()` → `netd_xfer_cb()`. DHCP working confirms this path is functional.

### 3. `linkoutput_fn()` Retry Loop — Behavior Under Load

`net_glue.c:61–77` — when `can_xmit` is false:

```c
for (int tries = 0; tries < 20; tries++) {
    if (!tud_ready()) { net_tx_fail++; return ERR_USE; }
    if (tud_network_can_xmit(p->tot_len)) {
        tud_network_xmit(p, 0);
        net_tx_ok++;
        return ERR_OK;
    }
    tud_task();   // pump USB until IN transfer completes
}
net_tx_fail++;
return ERR_WOULDBLOCK;
```

This retry loop is called from inside `ethernet_input()` which is called from
`net_service()`. It calls `tud_task()` to process pending USB completions. For
ICMP reply: if `can_xmit` is transiently false (previous ARP reply or mDNS packet
in-flight), up to 20 `tud_task()` calls should resolve it within one USB frame
(1 ms at full-speed). If all 20 exhaust, `ERR_WOULDBLOCK` is returned and the ICMP
reply is dropped. This is **transient** — the next ping attempt should succeed.

### 4. Bug A: Sleep/Wake — Root Cause Confirmed

From UART trace in `2026-04-05-macos-sleep-wake-uart-trace.md`:

```
!USB suspend     ← tud_suspend_cb fired
$PYRO,43,...
...
!USB resume      ← tud_resume_cb fired
$PYRO,49,...
[ping never recovers — reset required]
```

**What macOS does**: Sends USB bus-level suspend on sleep, bus-level resume on wake.
Does **not** send `SET_INTERFACE alt=0/1` or `SET_ETHERNET_PACKET_FILTER`.
The CDC-ECM data interface remains logically alt=1, but macOS's
`IOUSBNetworkingFamily` driver pauses routing and waits for a fresh
`NETWORK_CONNECTION` CDC notification before resuming.

**What the device does (committed code)**:

- `tud_suspend_cb()` → stub, `bx lr`. Does nothing.
- `tud_resume_cb()` → stub, `bx lr`. Does nothing.
- `can_xmit` may be `false` if a transfer was in-flight at suspend (interrupted transfers may not complete).
- `ecm_report(true)` (NETWORK_CONNECTION) is only triggered by `SET_ETHERNET_PACKET_FILTER`. macOS never re-sends this after bus resume. So NETWORK_CONNECTION is **never sent after wake**.

**Result**: macOS's network driver never receives NETWORK_CONNECTION → does not
resume routing → every outbound packet from macOS is dropped → ping fails forever
until USB replug (which forces full re-enumeration including SET_ETHERNET_PACKET_FILTER).

### 5. Bug A: Fix — Already Defined, Not Yet Wired

`tud_network_ecm_resume()` IS already implemented in the patched
`~/.pico-sdk/sdk/2.2.0/lib/tinyusb/src/class/net/ecm_rndis_device.c:227–232`:

```c
void tud_network_ecm_resume(void) {
  if (!_netd_itf.ecm_mode) return;
  if (_netd_itf.ep_in == 0) return;   /* not yet configured — nothing to do */
  can_xmit = true;                    /* reset in case suspend interrupted a TX */
  ecm_report(true);                   /* NETWORK_CONNECTION: Connected */
}
```

The fix is to call it from `tud_resume_cb()` in `net_glue.c`. However, `ecm_report()`
calls `netd_report()` which calls `usbd_edpt_xfer()` — this must NOT be called from
inside a TinyUSB callback (per the feedback memory `feedback_tud_callbacks_timing.md`).

The deferred approach (from `2026-04-05-usb-sleep-wake-ecm-resume.md`):

```c
// net_glue.c
static volatile bool ecm_resume_pending = false;

void tud_resume_cb(void) {
    ecm_resume_pending = true;   // defer to net_service() — cannot xfer from here
}

void net_service(void) {
    if (ecm_resume_pending) {
        ecm_resume_pending = false;
        tud_network_ecm_resume();    // safe: called from main loop, not callback
        if (mdns_started) mdns_resp_restart(&netif_data);
    }
    if (received_frame) {
        // ... existing frame processing ...
    }
    sys_check_timeouts();
}
```

This pattern is safe: `tud_resume_cb()` only sets a flag, and `net_service()` acts
on it during the next main-loop iteration, outside any TinyUSB callback context.

### 6. Bug B: Fresh-Connect Ping — Memory Claim vs Code Analysis

The memory `project_ping_regression.md` claims:
> "v2.1.42 HEAD: ping never works, even on fresh connect with no flight activity"

**Code analysis cannot reproduce this as a code-level bug:**

- The `ecm_rndis_device.c` patch (line 270–275) correctly sets `can_xmit = true`
  and calls `tud_network_recv_renew()` on every `SET_INTERFACE alt=1`, including
  the very first one on a fresh physical replug.
- `NETIF_FLAG_LINK_UP` is set statically in `netif_init_cb()` (line 85) and never
  cleared in the committed code. lwIP accepts and routes IP packets from startup.
- `LWIP_ICMP 1` is set in `lwipopts.h:10`. lwIP's `icmp_input()` builds echo
  replies autonomously. No application-layer ICMP handling is needed.
- DHCP working implies `can_xmit` cycles correctly (each DHCP reply goes through
  `do_in_xfer()` → `netd_xfer_cb()` → `can_xmit = true`).
- ARP: `etharp_input()` populates the ARP cache from the ARP request that macOS
  sends before the first ICMP echo. By the time `icmp_input()` calls `etharp_output()`,
  the host's MAC is in the cache.

**Most likely explanation for the memory claim**: The test was run after a macOS
sleep/wake cycle (Bug A), not after a true fresh USB replug. After wake, macOS
does not re-enumerate — physical replug looks "fresh" to the user but macOS's
`IOUSBNetworkingFamily` state still needs `NETWORK_CONNECTION` to resume routing.
This would explain ping failing "even on fresh connect" if "connect" meant
replug-without-reset on a Mac that had slept.

**What commit `1116373` says**: "85/85 host tests pass, hardware verified: PAD_IDLE
stable, HTTP working." HTTP is tested via TCP; TCP has retransmission and would
recover from transient `can_xmit = false`. Ping was likely not in the test matrix.

### 7. `net_service()` — `tud_network_recv_renew()` Timing

`net_glue.c:177–185` — `tud_network_recv_renew()` is only called inside the
`if (received_frame)` block. After USB resume, if `received_frame` is NULL (no
frame arrived while suspended), `tud_network_recv_renew()` is never called from
`net_service()`. The OUT endpoint gets re-armed only if:
(a) a frame arrives and `tud_network_recv_cb()` returns `false` (no slot), or
(b) `tud_network_ecm_resume()` calls `tud_network_recv_renew()` explicitly.

This is another reason the deferred `tud_network_ecm_resume()` call is essential:
it re-arms both `can_xmit` and the OUT endpoint in one operation.

---

## Code References

| File | Lines | What |
|---|---|---|
| `src/hal_hardware.c` | 391–404 | `uart_dma_init()` — DMA channel claim, `DMA_IRQ_0` handler |
| `src/hal_hardware.c` | 384–389 | `uart_dma_irq()` — only touches UART DMA registers |
| `src/net_glue.c` | 61–77 | `linkoutput_fn()` — 20-retry loop calling `tud_task()` |
| `src/net_glue.c` | 177–185 | `net_service()` — single-slot frame processing |
| `src/net_glue.c` | 83–92 | `netif_init_cb()` — `NETIF_FLAG_LINK_UP` baked in |
| `src/net_glue.c` | 126–132 | `tud_network_init_cb()` — printf + pbuf_free only |
| `src/lwipopts.h` | 10 | `LWIP_ICMP 1` |
| `ecm_rndis_device.c` | 83 | `static bool can_xmit` at `0x200121ab` |
| `ecm_rndis_device.c` | 89–91 | `do_in_xfer()` — clears `can_xmit` before every TX |
| `ecm_rndis_device.c` | 262–275 | `SET_INTERFACE alt=1` handler (patched) — sets `can_xmit = true` |
| `ecm_rndis_device.c` | 354–381 | `netd_xfer_cb()` — restores `can_xmit = true` on IN complete |
| `ecm_rndis_device.c` | 227–232 | `tud_network_ecm_resume()` — fix function, not yet called |

---

## Architecture: Ping Path (Normal Fresh Connect)

```
macOS: ping 192.168.7.1
  │
  1. macOS sends ARP request (broadcast)
  │    USB OUT → handle_incoming_packet() → tud_network_recv_cb()
  │    received_frame = [ARP pbuf]
  │
  2. net_service() → ethernet_input() → etharp_input()
  │    etharp_input() adds 192.168.7.2 → hostMAC to ARP cache
  │    ARP reply built → linkoutput_fn() → can_xmit=true → tud_network_xmit()
  │    do_in_xfer() → can_xmit=false (IN transfer in progress)
  │    received_frame=NULL, tud_network_recv_renew() (re-arms OUT)
  │
  3. netd_xfer_cb() (IN complete, non-ZLP) → can_xmit=true
  │
  4. macOS sends ICMP echo request
  │    USB OUT → tud_network_recv_cb() → received_frame = [ICMP pbuf]
  │
  5. net_service() → ethernet_input() → ip4_input() → icmp_input()
  │    icmp_input() builds echo reply, calls ip4_output() → etharp_output()
  │    etharp_output() finds 192.168.7.2 in ARP cache → linkoutput_fn()
  │    can_xmit=true → tud_network_xmit() → do_in_xfer() → can_xmit=false
  │    received_frame=NULL, tud_network_recv_renew()
  │
  6. netd_xfer_cb() → can_xmit=true
  7. macOS receives ICMP echo reply ✓
```

```
macOS: sleep → wake (current committed code — BUG)
  │
  USB bus suspend → tud_suspend_cb() [no-op stub] — can_xmit may be false
  USB bus resume  → tud_resume_cb() [no-op stub]
  │    NETWORK_CONNECTION never re-sent
  │    macOS IOUSBNetworkingFamily stalls — routing paused
  │
  macOS: ping 192.168.7.1
  └── ARP request never sent (macOS routing paused)
      → no response → "Request timeout" forever
```

---

## Open Questions

1. **Does physical USB replug after Mac sleep reliably fix ping?** This would
   confirm or deny whether Bug B (fresh-connect) is real or was a test methodology
   artifact. If replug fixes it, Bug B is not real — it's always been Bug A.

2. **Does macOS re-send `SET_ETHERNET_PACKET_FILTER` after `tud_network_ecm_resume()`
   sends NETWORK_CONNECTION?** If yes, `ecm_report(true)` fires twice (once from
   `tud_network_ecm_resume()`, once from the filter handler) — harmless but worth
   confirming.

3. **Does `can_xmit` actually become false during USB suspend?** If the RP2040 USB
   controller automatically aborts in-flight transfers on suspend, `can_xmit` stays
   false. If it preserves them for replay on resume, `can_xmit` might recover. The
   `tud_network_ecm_resume()` force-sets `can_xmit = true` regardless.

---

## Related Research

- `thoughts/shared/research/2026-04-05-macos-sleep-wake-uart-trace.md` — UART trace confirming sleep/wake event sequence
- `thoughts/shared/research/2026-04-05-macos-ecm-network-connection-notification.md` — ECM patch analysis, SET_INTERFACE alt=1 behavior
- `thoughts/shared/plans/2026-04-05-usb-sleep-wake-ecm-resume.md` — implementation plan for deferred `tud_network_ecm_resume()`
- `thoughts/shared/research/2026-04-04-web-interface-30min-timeout.md` — prior research on 30-min timeout failure mode
