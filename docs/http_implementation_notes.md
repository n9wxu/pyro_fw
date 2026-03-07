# HTTP Interface Implementation Notes

## Status: COMPLETE (v1.2.0)

## Architecture
USB composite device (ECM/RNDIS + vendor reset) with lwIP TCP/IP stack, custom HTTP server, mDNS, and DNS-SD. Based on TinyUSB's net_lwip_webserver example, heavily modified.

## Key Files
- `src/http_server.c` — Custom streaming HTTP server with littlefs
- `src/net_glue.c` — TinyUSB ↔ lwIP bridge, DHCP, DNS, mDNS
- `src/usb_descriptors.c` — USB composite: ECM/RNDIS + vendor reset
- `src/reset_interface.c` — Vendor reset for picotool
- `src/tusb_config.h` — TinyUSB configuration
- `src/lwipopts.h` — lwIP configuration
- `src/arch/cc.h` — lwIP platform hooks

## lwIP Configuration
- `MEMP_NUM_TCP_PCB=16` — supports browser parallel connections + mDNS
- `MEM_SIZE=8000` — heap for TCP segment coalescing
- `PBUF_POOL_SIZE=24` — packet buffers
- `MEMP_NUM_SYS_TIMEOUT=16` — timers for mDNS
- `LWIP_MDNS_RESPONDER=1` — mDNS with DNS-SD
- `LWIP_IGMP=1` — multicast for mDNS

## HTTP Server Design
- 8-slot connection pool (`conn_state_t`)
- Streaming file reads (512-byte chunks, fills TCP send buffer)
- `tcp_err` callback prevents connection pool leaks
- `on_sent` callback chain for multi-chunk file transfers
- Handles ERR_MEM by rewinding file position and retrying

## USB Configuration
- VID: 0x2E8A (Raspberry Pi), PID: 0x4002
- Two configurations: RNDIS (Windows) and ECM (macOS/Linux)
- Vendor reset interface for picotool (deferred to main loop)
- No CDC — removed to avoid macOS driver conflicts with ECM
