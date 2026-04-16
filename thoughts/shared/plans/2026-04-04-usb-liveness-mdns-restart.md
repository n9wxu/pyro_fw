# USB Liveness, Link State, and mDNS Restart Implementation Plan

## Overview

Three related changes to make the device more observable and to correctly model USB
connectivity in the network stack:

1. **LED proof-of-life**: Toggle GPIO 25 (onboard LED) on every main loop iteration so
   there is a physical, hardware-visible heartbeat that confirms the firmware is executing.
2. **USB link state hooks**: Implement the four TinyUSB device callbacks
   (`tud_mount_cb`, `tud_umount_cb`, `tud_suspend_cb`, `tud_resume_cb`) so lwIP's
   `NETIF_FLAG_LINK_UP` tracks actual USB connectivity instead of being permanently set.
3. **mDNS restart on reconnect**: When the USB host re-initializes the network interface
   (`tud_network_init_cb`), call `mdns_resp_restart()` so the device re-probes and
   re-announces `pyro.local` without requiring a firmware reboot.

## Current State Analysis

- **GPIO 25**: No existing initialization or use anywhere in the firmware.
  `hardware/gpio.h` is already included in `hal_hardware.c:11`.
- **Main loop** (`main_hardware.c:61–83`): Six statements inside `while(1)`.
  No existing toggle or blink logic.
- **TinyUSB callbacks**: Only `tud_network_init_cb()` exists (`net_glue.c:126`).
  `tud_mount_cb`, `tud_umount_cb`, `tud_suspend_cb`, `tud_resume_cb` are absent —
  TinyUSB's weak no-op defaults apply.
- **lwIP link state**: `NETIF_FLAG_LINK_UP` is baked into the static flags field in
  `netif_init_cb()` at `net_glue.c:85` and is never cleared. lwIP functions
  `netif_set_link_up()` / `netif_set_link_down()` exist in `lwip/netif.h:488–489`
  but are not called anywhere.
- **mDNS**: `mdns_resp_restart(struct netif *)` is available in `lwip/apps/mdns.h:122`.
  `mdns_started` is a static bool in `net_glue.c:155` — accessible within the same file.
  `tud_network_init_cb()` currently only clears `received_frame`.

## Desired End State

- The onboard LED blinks at the main loop rate, providing hardware proof of firmware
  execution visible without a debugger or serial connection.
- `netif_is_link_up()` returns true only when USB is connected and configured; false
  when disconnected or suspended. This is the correct lwIP semantic for a USB network
  adapter.
- After USB replug (or any host-initiated reconnect), `pyro.local` resolves correctly
  within the mDNS TTL window without requiring a firmware reboot or physical USB replug.

### Verification:
- LED blinks continuously at power-on, even before USB is connected.
- `ping 192.168.7.1` fails while USB is disconnected (link down).
- `ping 192.168.7.1` succeeds within a few seconds of USB replug.
- `ping pyro.local` resolves correctly within ~5 seconds of USB replug.
- Web interface at `http://pyro.local` loads after replug without firmware reboot.

## What We're NOT Doing

- No change to the DHCP or DNS server (those run independently of link state).
- No change to the `NETIF_FLAG_UP` administrative flag — the interface stays
  administratively up at all times; only the link flag tracks USB state.
- No change to the HTTP server, TCP stack, or connection pool.
- No watchdog integration.
- No mDNS hostname suffix reset — existing conflict-resolution behavior is unchanged.

## Implementation Approach

All changes fit in two files: `hal_hardware.c` / `main_hardware.c` for the LED, and
`net_glue.c` for the USB callbacks and mDNS restart. No new files, no new headers,
no CMakeLists changes.

---

## Phase 1: LED Proof-of-Life

### Overview

Initialize GPIO 25 as a push-pull output during platform init, then toggle it every
main loop iteration using `gpio_xor_mask()` (single instruction, no read-modify-write).

### Changes Required

#### 1.1 GPIO Initialization

**File**: `src/hal_hardware.c`
**Location**: `hal_platform_init()`, after the buzzer init block and before `board_init()`
(approximately line 578).

```c
/* Proof-of-life LED — GPIO 25 (onboard LED on Pico).
 * Toggled every main loop iteration in main_hardware.c. */
gpio_init(25);
gpio_set_dir(25, GPIO_OUT);
gpio_put(25, 0);
```

#### 1.2 Main Loop Toggle

**File**: `src/main_hardware.c`
**Location**: First statement inside `while(1)`, before `hal_time_ms()` (line 62).

Add the required include at the top of the file if not already present:
```c
#include "hardware/gpio.h"
```

Add the toggle at the top of the loop:
```c
while (1) {
    gpio_xor_mask(1u << 25);   /* proof-of-life heartbeat */

    uint32_t now = hal_time_ms();
    /* ... rest of loop unchanged ... */
}
```

### Success Criteria

#### Automated Verification:
- [x] Firmware builds without errors or warnings: `cmake --build build`

#### Manual Verification:
- [ ] LED blinks continuously at a visible rate immediately after power-on.
- [ ] LED continues blinking when USB is disconnected.
- [ ] LED continues blinking when the web interface is loaded.

**Pause here for manual confirmation before proceeding to Phase 2.**

---

## Phase 2: USB Link State Hooks and mDNS Restart

### Overview

Three coordinated changes, all in `src/net_glue.c`:

1. Remove `NETIF_FLAG_LINK_UP` from the static netif init so the link starts down.
2. Add the four TinyUSB device callbacks to drive `netif_set_link_up()` /
   `netif_set_link_down()`.
3. Extend `tud_network_init_cb()` to call `mdns_resp_restart()` when the USB host
   re-initializes the network interface.

### Changes Required

#### 2.1 Remove Static `NETIF_FLAG_LINK_UP`

**File**: `src/net_glue.c`
**Location**: `netif_init_cb()`, line 85.

```c
/* Before */
netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP | NETIF_FLAG_IGMP;

/* After */
netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_UP | NETIF_FLAG_IGMP;
```

The link-up flag will now be set dynamically by `tud_mount_cb()` when the USB host
completes enumeration.

#### 2.2 Add TinyUSB Device Callbacks

**File**: `src/net_glue.c`
**Location**: Add after `tud_network_init_cb()` (after line 132).

```c
/* ── TinyUSB device lifecycle callbacks ──────────────────────────── */

void tud_mount_cb(void) {
    /* USB host completed enumeration (SET_CONFIGURATION received). */
    netif_set_link_up(&netif_data);
}

void tud_umount_cb(void) {
    /* USB host unconfigured or disconnected. */
    netif_set_link_down(&netif_data);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    /* USB bus suspended — treat as link down. */
    netif_set_link_down(&netif_data);
}

void tud_resume_cb(void) {
    /* USB bus resumed. */
    netif_set_link_up(&netif_data);
}
```

These override TinyUSB's weak no-op defaults. `netif_data` is the file-scope
`struct netif` already used throughout `net_glue.c`.

#### 2.3 mDNS Restart in `tud_network_init_cb()`

**File**: `src/net_glue.c`
**Location**: `tud_network_init_cb()`, lines 126–132.

```c
/* Before */
void tud_network_init_cb(void) {
    lwip_uart_printf("!NET init_cb\r\n");
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
}

/* After */
void tud_network_init_cb(void) {
    lwip_uart_printf("!NET init_cb\r\n");
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
    if (mdns_started) {
        mdns_resp_restart(&netif_data);
    }
}
```

`mdns_started` is already declared `static bool` in the same file (line 155).
`mdns_resp_restart()` is declared in `lwip/apps/mdns.h` which is already transitively
included via the lwIP build target.

### Success Criteria

#### Automated Verification:
- [ ] Firmware builds without errors or warnings: `cmake --build build`

#### Manual Verification:
- [ ] With device connected and web interface working, unplug USB. `ping 192.168.7.1`
  gets no response within 2 seconds of unplug.
- [ ] Replug USB. `ping 192.168.7.1` succeeds within 5 seconds of replug.
- [ ] `ping pyro.local` resolves and succeeds within 10 seconds of replug.
- [ ] Web interface at `http://pyro.local` loads fully (HTML + JS) after replug,
  without firmware reboot.
- [ ] LED continues blinking throughout all of the above.

---

## Testing Strategy

### Manual Testing Steps

1. Flash firmware. Verify LED blinks immediately.
2. Connect USB. Wait for IP assignment. Verify `ping 192.168.7.1` succeeds and web
   interface loads at `http://192.168.7.1` and `http://pyro.local`.
3. Unplug USB while watching LED. Verify LED keeps blinking (firmware alive).
   Verify `ping 192.168.7.1` gets no response.
4. Replug USB. Verify `ping 192.168.7.1` recovers. Verify `pyro.local` resolves.
   Verify full web page loads (including `app.js`, not just `index.html`).
5. Repeat steps 3–4 three times to confirm consistent behavior.
6. Leave connected for 30+ minutes. Verify web interface continues responding.

## References

- Research: `thoughts/shared/research/2026-04-04-web-interface-30min-timeout.md`
- `src/net_glue.c:85` — `NETIF_FLAG_LINK_UP` static init (to remove)
- `src/net_glue.c:126–132` — `tud_network_init_cb()` (to extend)
- `src/net_glue.c:155` — `mdns_started` static bool
- `src/hal_hardware.c:574` — `hal_platform_init()` (add GPIO 25 init)
- `src/main_hardware.c:61` — `while(1)` loop (add toggle)
- lwIP `netif_set_link_up/down`: `~/.pico-sdk/sdk/2.2.0/lib/lwip/src/include/lwip/netif.h:488–489`
- lwIP `mdns_resp_restart`: `~/.pico-sdk/sdk/2.2.0/lib/lwip/src/include/lwip/apps/mdns.h:122`
