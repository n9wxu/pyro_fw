# HTTP Interface Implementation Notes

## Status: in service (2.1.702, 2026-09-28)

## Architecture
USB composite device (ECM/RNDIS + vendor reset) with lwIP TCP/IP stack, custom HTTP server, mDNS, and DNS-SD, and the board's own DHCP and DNS servers. Based on TinyUSB's net_lwip_webserver example, heavily modified.

The HTTP server is two halves (DD-039, DD-061):
- **The transport**, `http_server_transport()`, called from `net_service()`, moves bytes between lwIP and each connection's rx and tx rings and decides when to close. It runs no handler. lwIP callbacks only queue.
- **Work units** do everything else: parse, route, render, read a file, write flash. Core0 runs them from the loop's slack, after the flight work, one at a time and in turn, starting one only with `HTTP_UNIT_BUDGET_US` (2 ms) of the period left, except the first of each period (WEB-HTTP-06). The `/api/status` render is portable: a render core0 had no room for, core1 runs with its next grant, before its Lua slice (WEB-HTTP-07). Ownership is whole-connection and changes hands only on core0, so the rings need no cross-core protocol.

## Key Files
- `src/http_server.c` — Routes and the lwIP adapter
- `src/http_conn.c`, `src/http_conn.h` — One HTTP exchange over a byte stream: parser, rings, response framing
- `src/http_work.c`, `src/http_work.h` — Work units, and who runs each
- `src/status_json.c` — The `/api/status` render, from a snapshot core0 takes in one pass (WEB-API-11)
- `src/net_ring.c`, `src/net_ring.h` — The byte rings
- `src/net_glue.c` — TinyUSB ↔ lwIP bridge, DHCP, DNS, mDNS
- `src/net_txq.c` — Frames the USB endpoint cannot take yet, held in order (DD-070)
- `src/net_stats.c` — The `/api/net` report (WEB-API-13)
- `src/flash_window.h` — When a request may write flash
- `src/usb_descriptors.c` — USB composite: ECM/RNDIS + vendor reset
- `src/reset_interface.c` — Vendor reset for picotool
- `src/tusb_config.h` — TinyUSB configuration
- `src/lwipopts.h` — lwIP configuration
- `src/arch/cc.h` — lwIP platform hooks

## lwIP Configuration
- `MEMP_NUM_TCP_PCB=16` — supports browser parallel connections + mDNS
- `MEM_SIZE=8000` — heap for TCP segment coalescing. Two or three connections streaming at once can exhaust it (DD-070)
- `TCP_SND_BUF` and `TCP_WND` = 4 × `TCP_MSS`, 5840 bytes
- `PBUF_POOL_SIZE=24` — packet buffers
- `MEMP_NUM_SYS_TIMEOUT=16` — timers for mDNS
- `LWIP_MDNS_RESPONDER=1` — mDNS with DNS-SD
- `LWIP_IGMP=1` — multicast for mDNS
- `LWIP_DHCP=0` — the board serves DHCP; it is not a client

## HTTP Server Design
- A link per lwIP pcb, up to `MEMP_NUM_TCP_PCB`; an exchange (`conn_t`) per request in progress, four of them (`CONN_POOL_SIZE`). A link gets an exchange when it first sends something, oldest first. While all four are busy, what a link receives waits in its pbuf queue, unacknowledged, so TCP flow control holds the sender back
- Each exchange parses from a 2 KB rx ring and writes to a 2 KB tx ring, with a 5 KB work buffer for a gathered body, a rendered response or a littlefs file cache (`http_conn.h`). Nothing sees a segment: a request split at any byte parses the same (WEB-HTTP-01)
- The TCP window reopens by what the parser took, not by what arrived, so a body waiting for the flash window holds its sender back (WEB-HTTP-03)
- Every response is framed by Content-Length and carries Connection: close: one request per connection (WEB-HTTP-02). Files and the flight CSV stream through the `fill` handler; the CSV's length is counted a unit at a time before the head is sent
- A malformed or oversized request gets its status: 400, 405, 411, 413, 414 or 431 (WEB-HTTP-04)
- `tcp_err` frees the link and releases its exchange; a link that moves no byte for 20 s is aborted
- A request that writes flash holds the flash window and waits for it with its bytes in the ring (`flash_window.h`). Uploads, the Lua program, config, pins, beeps, the serial, the log erase and OTA do
- From launch until the flight log closes, the log alone holds littlefs: a request that needs the filesystem answers 423, and one that held it at launch is reset (WEB-API-08, DD-058). The rest of the API stays live
- A frame the USB endpoint cannot take yet is held, in a queue of eight, and sent in order as the endpoint frees (DD-070)

## Routes
- `GET /` — `/www/index.html`, or a default page when none is uploaded
- `GET /<path>` — any littlefs file
- `GET /api/status` — JSON status (WEB-API-01)
- `GET|POST /api/config` — `config.ini`; a POST merges into the running config and applies it, in PAD_IDLE only (CFG-06)
- `GET|POST /api/pins`, `GET /api/pins/caps` — the pin assignment, applied at the next reboot, and what the board offers; a POST in PAD_IDLE only
- `GET|POST /api/beeps` — the beep table; `POST /api/beeps/play` — one sound, played once, in PAD_IDLE only
- `GET /api/flight.csv`, `POST /api/flight/erase`, `GET /api/log/space` — the flight log, rendered as CSV from its binary records (WEB-API-06, WEB-API-09, WEB-API-12)
- `GET /api/pressure/trace?since=N` — every pressure conversion since N, in binary (SNS-PRES-13)
- `GET /api/net` — lwIP's pools, TCP's connections and what the transport refused (WEB-API-13)
- `POST /api/test_mode/on`, `POST /api/test_mode/off` — fly on USB as on battery; held in RAM (USB-08)
- `POST /api/ota`, `POST /api/reboot` — a firmware image into the download slot, then a reboot once the answer is with lwIP (WEB-API-04, WEB-API-05)
- `POST /api/serial` — the board's serial, which is its MAC, applied at the next reboot
- `POST /www/<file>` — a web file upload
- `GET|POST /api/lua/script`, `POST /api/lua/check`, `GET /api/lua/console` — Lua boards only

## USB Configuration
- VID: 0x2E8A (Raspberry Pi), PID: 0x4002
- Two configurations: RNDIS (Windows) and ECM (macOS/Linux)
- The serial number and the ECM MAC string are the board's MAC, so `picotool --ser <mac>` targets one board, and the third octet of its subnet, 192.168.N.0/24, comes from it
- Vendor reset interface for picotool (deferred to main loop)
- No CDC (`CFG_TUD_CDC 0`), to avoid macOS driver conflicts with ECM
