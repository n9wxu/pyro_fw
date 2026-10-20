# HTTP Interface Implementation Notes

## Status: in service (2.1.702, 2026-09-28)

## Architecture
USB composite device (ECM/RNDIS + vendor reset) with the smallest_tcp TCP/IP stack (github.com/n9wxu/smallest_tcp, DD-102), a custom HTTP server, mDNS and DNS-SD, and the board's own DHCP server for its one host.

The HTTP server is two halves (DD-039, DD-061):
- **The transport**, `src/http_transport.c`, called from `net_service()`, moves bytes between the stack and each connection's rx and tx rings and decides when to close. It runs no handler. The stack's event callback only notes.
- **Work units** do everything else: parse, route, render, read a file, write flash. Core0 runs them from the loop's slack, after the flight work, one at a time and in turn, starting one only with `HTTP_UNIT_BUDGET_US` (2 ms) of the period left, except the first of each period (WEB-HTTP-06). The `/api/status` render is portable: a render core0 had no room for, core1 runs with its next grant, before its Lua slice (WEB-HTTP-07). Ownership is whole-connection and changes hands only on core0, so the rings need no cross-core protocol.

## Key Files
- `src/http_server.c` — Routes, and the exchanges the transport lends to links
- `src/http_transport.c` — Links over smallest_tcp: the stack's buffer operations over each exchange's rings
- `src/http_conn.c`, `src/http_conn.h` — One HTTP exchange over a byte stream: parser, rings, response framing
- `src/http_work.c`, `src/http_work.h` — Work units, and who runs each
- `src/status_json.c` — The `/api/status` render, from a snapshot core0 takes in one pass (WEB-API-11)
- `src/net_ring.c`, `src/net_ring.h` — The byte rings
- `src/net_glue.c` — TinyUSB ↔ smallest_tcp bridge, DHCP, mDNS
- `src/net_txq.c` — Frames the USB endpoint cannot take yet, held in order in 256 bytes (WEB-NET-05)
- `src/net_stats.c` — The `/api/net` report (WEB-API-13)
- `src/flash_window.h` — When a request may write flash
- `src/usb_descriptors.c` — USB composite: ECM/RNDIS + vendor reset
- `src/reset_interface.c` — Vendor reset for picotool
- `src/tusb_config.h` — TinyUSB configuration
- `src/net_shim/netif/ethernet.h` — the one constant TinyUSB's RNDIS replies take from lwIP's header

## smallest_tcp Configuration
- v0.1.12, fetched by CMake; IPv4 only; `-Os`
- `NET_API_PREFIX=stcp_` — the stack's external names under a prefix, so its `net_init()` and the firmware's are two functions (`src/net_stcp.h`)
- `MDNS_TIEBREAK=0` — no other host probes for the board's names on a point-to-point link
- No heap and no pools: two 1,514-byte frame buffers, the links' records and the 256-byte transmit queue

## HTTP Server Design
- Sixteen links (`HTTP_LINKS`), each one of the stack's connections listening on port 80; an exchange (`conn_t`) per request in progress, four of them (`CONN_POOL_SIZE`). A connected link gets an exchange in the order the links connected; until then its window stays shut, so TCP flow control holds the sender back. The stack has no accept queue: a SYN that finds every link in use is reset
- One segment in flight per link, copied from the tx ring, which keeps it until it is acknowledged. A part-segment waits for the handler to refill the ring, and the stack is shown one segment at a time so each carries PSH
- Each exchange parses from a 2 KB rx ring and writes to a 2 KB tx ring, with a 5 KB work buffer for a gathered body, a rendered response or a file cache (`http_conn.h`). Nothing sees a segment: a request split at any byte parses the same (WEB-HTTP-01)
- The TCP window reopens by what the parser took, not by what arrived, so a body waiting for the flash window holds its sender back (WEB-HTTP-03)
- Every response is framed by Content-Length and carries Connection: close: one request per connection (WEB-HTTP-02). Files and the flight CSV stream through the `fill` handler; the CSV's length is counted a unit at a time before the head is sent
- A malformed or oversized request gets its status: 400, 405, 411, 413, 414 or 431 (WEB-HTTP-04)
- A link whose peer is gone gives its exchange back; a link that moves no byte for 20 s is aborted
- A request that writes flash holds the flash window and waits for it with its bytes in the ring (`flash_window.h`). Uploads, the Lua program, config, pins, beeps, the serial, the log erase and OTA do
- From launch until the flight log closes, the log alone holds the filesystem: a request that needs the filesystem answers 423, and one that held it at launch is reset (WEB-API-08, DD-058). The rest of the API stays live
- A frame the USB endpoint cannot take yet is copied into 256 bytes and sent in order as the endpoint frees; a SYN, data or a FIN that does not fit the stack sends again at its next pass (WEB-NET-05)

## Routes
- `GET /` — `/www/index.html`, or a default page when none is uploaded
- `GET /<path>` — any file
- `GET /api/status` — JSON status (WEB-API-01)
- `GET|POST /api/config` — `config.ini`; a POST merges into the running config and applies it, in PAD_IDLE only (CFG-06)
- `GET|POST /api/pins`, `GET /api/pins/caps` — the pin assignment, applied at the next reboot, and what the board offers; a POST in PAD_IDLE only
- `GET|POST /api/beeps` — the beep table; `POST /api/beeps/play` — one sound, played once, in PAD_IDLE only
- `GET /api/flight.csv`, `POST /api/flight/erase`, `GET /api/log/space` — the flight log, rendered as CSV from its binary records (WEB-API-06, WEB-API-09, WEB-API-12)
- `GET /api/pressure/trace?since=N` — every pressure conversion since N, in binary (SNS-PRES-13)
- `GET /api/net` — TCP's links by state, retransmissions, what the transport refused and the frames each way (WEB-API-13)
- `POST /api/test_mode/on[?log=host]`, `POST /api/test_mode/off` — fly on USB as on battery; held in RAM (USB-08). With `?log=host` the flight log goes to RAM for the host, not to the flash (USB-09)
- `GET /api/flight/live?since=N` — the flight log held for the host from byte N: two little-endian offsets, the first byte's and the log's end, then the bytes (WEB-API-16)
- `POST /api/ota`, `POST /api/reboot` — a firmware image into the download slot, then a reboot once the answer is on its way (WEB-API-04, WEB-API-05)
- `POST /api/serial` — the board's serial, which is its MAC, applied at the next reboot
- `POST /www/<file>` — a web file upload
- `GET|POST /api/lua/script`, `POST /api/lua/check`, `GET /api/lua/console` — Lua boards only

## USB Configuration
- VID: 0x2E8A (Raspberry Pi), PID: 0x4002
- Two configurations: RNDIS (Windows) and ECM (macOS/Linux)
- The serial number and the ECM MAC string are the board's MAC, so `picotool --ser <mac>` targets one board, and the third octet of its subnet, 192.168.N.0/24, comes from it
- Vendor reset interface for picotool (deferred to main loop)
- No CDC (`CFG_TUD_CDC 0`), to avoid macOS driver conflicts with ECM
