---
name: Project Context - Pyro MK1B
description: Architecture and USB stack details for the Pyro MK1B flight controller relevant to TinyUSB debugging
type: project
---

Pyro MK1B flight controller using RP2040 (Pico). USB stack: TinyUSB from Pico SDK 2.2.0 (lib/tinyusb). Network class: CFG_TUD_ECM_RNDIS=1 (ECM for macOS/Linux, RNDIS for Windows) — NOT NCM. Two configurations in one device descriptor (bNumConfigurations=2): config 0 = RNDIS+Reset, config 1 = ECM+Reset. lwIP stack bridges to TinyUSB via net_glue.c. DHCP server at 192.168.7.1, mDNS hostname "pyro". Fixed MAC 02:02:84:00:6A:00.

Key files:
- src/usb_descriptors.c — descriptor tables, two configs (RNDIS/ECM), no suspend/resume callbacks defined
- src/net_glue.c — tud_network_recv_cb, tud_network_xmit_cb, tud_network_init_cb (resets received_frame on re-init)
- src/hal_hardware.c — hal_platform_service() calls tud_task()+net_service() every main loop; hal_sleep_until_event() is a no-op (WFE was disabled due to suspected USB TX blocking)
- src/tusb_config.h — CFG_TUD_ECM_RNDIS=1, CFG_TUD_NCM=0

**Why:** The question is about macOS sleep/wake disconnect. The device uses ECM (config index 1), which macOS selects. ECM class state and re-enumeration behavior are the focus.

**How to apply:** When suggesting fixes, work within the existing ECM+RNDIS dual-config architecture. No NCM class is used despite the question mentioning NCM.
