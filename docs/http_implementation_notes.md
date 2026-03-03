# HTTP Interface Implementation Notes

## Status: IN PROGRESS

## Architecture
Based on TinyUSB's net_lwip_webserver example, adapted for Pyro MK1B.

## Key Source References (in Pico SDK 2.2.0)
- Example: `lib/tinyusb/examples/device/net_lwip_webserver/`
- lwIP: `lib/lwip/`
- Networking helpers: `lib/tinyusb/lib/networking/` (DHCP server, DNS server, RNDIS)
- Net class driver: `lib/tinyusb/src/class/net/`

## What the example provides
- USB ECM+RNDIS composite network device
- lwIP TCP/IP stack initialization
- DHCP server (assigns 192.168.7.2 to host)
- DNS server (resolves custom hostname)
- lwIP httpd (static pages)
- Network interface bridge (TinyUSB ↔ lwIP)

## What we need to add
- Custom HTTP handler for file operations (replace lwIP httpd)
- littlefs integration for file storage
- Flight status API endpoint
- HTML templates for dashboard, file list, editor
- Integration with flight controller main loop

## Implementation Plan
1. Get USB network device working (ECM+RNDIS)
2. Get lwIP + DHCP + ping working
3. Add custom HTTP server with file listing
4. Add file download/upload
5. Add config editor
6. Add status dashboard
7. Integrate with flight controller

## tusb_config.h changes
- Remove: CFG_TUD_MSC
- Add: CFG_TUD_ECM_RNDIS or CFG_TUD_NCM
- Keep: CFG_TUD_CDC (for debug serial)

## New files needed
- src/lwipopts.h - lwIP configuration
- src/usb_descriptors.c - rewrite for net device
- src/http_server.c - custom HTTP handler with littlefs
- src/net_glue.c - TinyUSB ↔ lwIP bridge (from example)

## CMakeLists.txt additions
- lwIP source files (~30 .c files)
- TinyUSB networking helpers
- New source files
- Remove: fat_mimic dependency
