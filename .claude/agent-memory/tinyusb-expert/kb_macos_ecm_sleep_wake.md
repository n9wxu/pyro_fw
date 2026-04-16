---
name: macOS USB ECM Sleep/Wake Disconnect Pattern
description: Root causes and fixes for ECM USB network device disconnecting on long macOS sleep and not recovering after wake
type: project
---

## Pattern
Device works normally. Short macOS sleep (<~60s): suspend/resume, device survives. Long macOS sleep (>~60s): VBUS is physically removed by macOS power management, device goes through full disconnect+re-enumeration on wake. ECM network interface on macOS (AppleUSBCDCECMData/Control) does NOT automatically re-bind cleanly in certain failure modes.

## Root causes identified for this project

1. **tud_network_init_cb not resetting lwIP netif link state**: On re-enumeration macOS re-selects ECM config and sends SET_INTERFACE(alt=1) to activate the data interface. TinyUSB calls tud_network_init_cb(). The current implementation only frees received_frame. It does NOT call netif_set_link_down()/netif_set_link_up() on the lwIP netif. macOS may complete ARP for 192.168.7.2 against a stale ARP cache entry. The lwIP ARP cache also has a stale entry. Neither side converges.

2. **DHCP lease re-assignment**: The DHCP server (dhserver) has 24-hour leases. After re-enumeration macOS issues a fresh DHCP DISCOVER. If the dhserver does not reuse the same IP for the same MAC (it matches on hwaddr), the macOS network stack may get a new IP and invalidate existing connections.

3. **mDNS one-shot init**: net_mdns_poll() has a mdns_started guard that prevents re-registration after re-enumeration. After VBUS removal and re-enumeration, mdns_resp_add_netif() is never called again for the new interface state, so mDNS (pyro.local) stops working even if IP connectivity recovers.

4. **No tud_suspend_cb / tud_resume_cb defined**: Without these, TinyUSB uses weak no-op defaults. The device does not signal macOS with remote wakeup (not needed here) but more importantly the firmware has no hook to know a suspend/resume cycle occurred vs a full VBUS-off cycle.

5. **netif_init_cb sets NETIF_FLAG_LINK_UP permanently**: The lwIP netif is initialized with LINK_UP hardcoded. After VBUS removal and re-enumeration, the netif never transitions down/up, so lwIP does not re-trigger ARP/neighbor discovery.

6. **TUD_CONFIG_DESCRIPTOR bmAttributes = 0**: The `0` in TUD_CONFIG_DESCRIPTOR(..., 0, 100) sets bmAttributes. Bit 6 = Self-powered, Bit 5 = Remote Wakeup. Value 0 means neither bit set, which also means bit 7 (required by USB spec = 1) is NOT set. The correct value is 0x80 (bus-powered, no remote wakeup). This may cause some hosts to reject or misparse the configuration descriptor. macOS is tolerant but it is a spec violation.

## Fixes

### Fix 1: Correct bmAttributes in both config descriptors (spec compliance)
In usb_descriptors.c, change the `0` bmAttributes argument in both TUD_CONFIG_DESCRIPTOR calls to `0x80`:
```c
TUD_CONFIG_DESCRIPTOR(CONFIG_ID_RNDIS + 1, 3, 0, RNDIS_CONFIG_TOTAL_LEN, 0x80, 100),
TUD_CONFIG_DESCRIPTOR(CONFIG_ID_ECM + 1, 3, 0, ECM_CONFIG_TOTAL_LEN, 0x80, 100),
```

### Fix 2: tud_network_init_cb must reset lwIP link state
```c
void tud_network_init_cb(void) {
    lwip_uart_printf("!NET init_cb\r\n");
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
    // Signal lwIP that the link cycled — clears ARP cache, retriggers IGMP
    netif_set_link_down(&netif_data);
    netif_set_link_up(&netif_data);
}
```

### Fix 3: Fix mDNS re-registration after re-enumeration
Reset mdns_started in tud_network_init_cb (or in tud_umount_cb) so net_mdns_poll() re-registers:
```c
extern bool mdns_started; // make non-static or add a reset function
void tud_umount_cb(void) {
    mdns_started = false;
    // optionally: mdns_resp_remove_netif(&netif_data);
}
```

### Fix 4: Add tud_mount_cb and tud_umount_cb for observability
Even empty stubs with a UART log line let you confirm whether re-enumeration is actually happening vs the device just not getting VBUS back.

### Fix 5 (diagnostic): Confirm VBUS is actually cycling
On RP2040, VBUS presence can be read via the USB VBUS detect bit in USB_SIE_STATUS register (bit 11, VBUS_DETECTED). Log this in the main loop or in tud_suspend_cb to confirm whether macOS is doing a true power-off vs a suspend.

## macOS-specific behavior (verified macOS 13/14 Ventura/Sonoma)
- Sleep < ~60s: USB suspend (SOF stops, D+/D- held in J state, VBUS maintained). Device sees tud_suspend_cb.
- Sleep > ~60s: macOS instructs the XHC to cut VBUS to USB-A ports (not Thunderbolt/USB-C on Apple Silicon which are on a different power domain). RP2040 loses power entirely. On wake, XHC re-applies VBUS and the device must complete full enumeration from scratch.
- Apple Silicon (M1/M2/M3) Macs: USB-A ports are on a hub downstream of the Thunderbolt controller. Behavior is similar but the exact sleep threshold may differ.
- The AppleUSBCDC ECM driver (AppleUSBCDCECMControl + AppleUSBCDCECMData kexts on Intel, DriverKit on Apple Silicon) does re-attach after re-enumeration IF the device descriptors are clean and the SET_INTERFACE sequence completes correctly.

## Known TinyUSB issue
TinyUSB ECM/RNDIS netd driver (src/class/net/ecm_rndis_device.c): after SET_INTERFACE(alt=1) the class driver calls tud_network_init_cb() but does NOT send an ECM connection speed notification or connection notification. macOS AppleUSBCDCECMControl expects a CDC NETWORK_CONNECTION notification (0x00) on the notification endpoint before it marks the link as up. Without it, macOS may show the interface as "connected" in System Settings but route no traffic. The TinyUSB ECM example works around this because macOS is lenient with this notification on initial enumeration but may be stricter on re-enumeration. Check if the notification endpoint (EP 0x81) is sending the connection notification after re-init.

[CATEGORY] macOS-ECM-SLEEP | Long sleep VBUS cycle breaks ECM re-attach: lwIP link state not reset, mDNS not re-registered, bmAttributes=0x00 spec violation | src/net_glue.c, src/usb_descriptors.c | 2026-04-05
