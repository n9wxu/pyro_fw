# Evaluating a switch from lwIP to smallest_tcp

Written 2026-10-04 against pyro_fw v2.2.5 and smallest_tcp 0.1.10
(github.com/n9wxu/smallest_tcp, commit 9293992). Nothing was built or
changed for this note; the figures come from the MK1B link map, from
smallest_tcp's documents, and from compiling its sources for Cortex-M0+.

## The short answer

The switch is worth making, and not yet. It would give back about 45 kB of
RAM and about 25 kB of flash, and remove the heap whose exhaustion is the
open item N1. Two things stand in the way today:

1. smallest_tcp sends one segment per round trip. Against a host that delays
   its acknowledgements, a file served by the board would move at 7 to 36 kB
   a second. A megabyte of flight log would take half a minute to two
   minutes.
2. smallest_tcp has never run on hardware, and has no USB driver.

Neither is a reason to stay on lwIP for good. Both are work in smallest_tcp
that this firmware would be the first to need.

Meanwhile lwIP's configuration is wasteful, and two open items (N1 and
RAM-1) can be closed this week without changing stacks.

**Since measured:** the spike below ran smallest_tcp on the MK1C-SD. On
macOS the first obstacle does not appear, and downloads run at about 90 % of
lwIP's speed. See "Spike results" at the end.

## What the firmware asks of a stack

| Need | Where | lwIP today | smallest_tcp 0.1.10 |
|---|---|---|---|
| Ethernet frames over USB (ECM and RNDIS) | `src/net_glue.c`, `src/net_txq.c` | netif glue, about 150 lines | no driver; the six-call MAC interface (`include/net_mac.h`) or `eth_input()` directly |
| ARP, IPv4, ICMP echo | lwIP core | yes | yes |
| DHCP server for the one host | TinyUSB `dhserver.c` on lwIP UDP | two leases | `dhcpv4_server.c`: one address, one client, written for this use |
| DNS answer for `pyro.local` | TinyUSB `dnserver.c` | yes | none; about 60 lines on its UDP, or rely on mDNS |
| mDNS name and `_pyro._tcp` service, renamed on conflict | lwIP mdns app | yes | `mdns.c`: A, PTR, SRV, TXT, probing, conflict callback |
| TCP: 4 exchanges served, more connections waiting | `src/http_server.c` adapter, about 290 lines | 16 pcbs, callbacks | application-declared connections, polled; no accept queue |
| Receive window follows the rx ring | WEB-HTTP-03 | `tcp_recved()` | native: the window is the ring's free space |
| Send from the tx ring | adapter | `tcp_write` copy into lwIP's heap | native: `next_segment()` reads the ring; bytes leave on ACK |
| `/api/net` counters | WEB-API-13 | lwIP's pools and pcb states | the keys change; the stack has no pools to report |

The HTTP engine (`http_conn.c`, `net_ring.c`) calls no lwIP and has its own
host suite. `net_ring.h` was written to smallest_tcp's buffer operations
(DD-039). The work is the adapter and the glue, about 450 lines replaced.

## What the switch would give

Measured on the MK1B image, lwIP against the smallest_tcp modules this
firmware would link (IPv4 only, `-Os`, Cortex-M0+, before section garbage
collection):

| | lwIP today | smallest_tcp | Difference |
|---|---|---|---|
| Flash, code | 40,786 B, plus up to 7,936 B of strings | 21,689 B | about 25 kB less |
| Flash, TinyUSB DHCP and DNS servers | 1,918 B | in the figure above (DNS extra) | |
| RAM | 50,761 B | about 3.6 kB (two frame buffers, the connection records) | about 45 kB less |

- **N1 goes away.** There is no heap. A connection's memory is its two
  rings, which exist already.
- **RAM-1 goes away.** MK1B has 880 bytes spare and MK1C about 700.
- **One copy fewer each way.** A received frame is parsed in place and its
  bytes go straight to the ring.
- **The model matches the engine.** No callbacks reaching into a connection;
  the net task polls.
- **Tests.** smallest_tcp has 1,408 host tests and a conformance suite; the
  adapter could be tested on the host against the real stack, which the
  lwIP adapter never was.

## What stands in the way

### 1. One segment in flight (decisive)

`tcp_buf_saw.c` is the only transmit buffer shipped, and `tcp.c` itself
allows one unacknowledged segment. The stack's own design note says a
peer "may hold every ACK for its full delayed-ACK time" and that for bulk
sending from the device this is the bottleneck.

By arithmetic, not measurement: 1,460 bytes per acknowledgement is 36 kB/s
at 40 ms and 7 kB/s at 200 ms.

| Served today | Size | At 7 to 36 kB/s |
|---|---|---|
| `app.js` | 74 kB | 2 to 10 s |
| A flight log | 1 MB and more | 30 s to over 2 minutes |
| A high-rate log from the card | 4 MB | 2 to 10 minutes |

Uploads are not affected: the board acknowledges at once, so an OTA keeps
today's speed.

Whether macOS, Linux and Windows do delay the ACK of a lone full segment on
this link is not known. It decides whether this is a real obstacle or a
theoretical one, and it can be measured in an afternoon (option D).

The cure is the ring transmit buffer already designed in smallest_tcp's
`tcp-buffer.md` section 5: several segments in flight, go-back-N on loss.
Its note lists congestion control and a measured RTO as prerequisites. On a
point-to-point USB link with no loss and no competing traffic those two
could be left out by configuration.

### 2. A busy endpoint is a lost segment

TCP ignores what the driver's `send` returns, so a frame the USB endpoint
cannot take is recovered by the retransmit timer: 1 s, doubling, and the
value never comes back down for that connection. DD-070 measured 2.4 % of
frames refused at the endpoint after its fix. The driver must therefore
never refuse: it keeps the queue `net_txq` has today, holding copies of
frames. With one segment in flight per connection that queue is short.

### 3. No accept queue

A SYN that finds no listening connection is answered with a reset. Today 16
connections may be open against 4 exchanges, the rest held by flow control;
`http_stream_check.py` opens 9. The equivalent is 16 connection records
(104 bytes each) whose receive window is zero until an exchange is free.
That needs a small receive-buffer implementation of its own, not the
shipped one.

### 4. Never run on hardware

The only board port (STM32F4) has not run. The version is 0.1.x and the API
may change. Against that: the author is the user, the test base is large,
and this firmware has four boards and a set of bench scripts that would
find faults quickly.

### 5. Smaller differences

- `/api/net`, `net_stats.c`, `api_check.py` and `test_network.py` name
  lwIP's pools and states. They change with the stack.
- No keep-alive and no measured RTO. The engine's own 20 s idle limit covers
  the first; a loss-free link covers the second.
- The DHCP server serves one client. TinyUSB's serves two addresses today;
  one host is all there is.
- `tcp_window_update()` must be called after the engine reads the ring.
- The event callback may not send or close; the adapter polls instead.

## Options

| | What | Cost | Result |
|---|---|---|---|
| A | **Stay on lwIP and tune it.** Shrink the receive pool, define `LWIP_NOASSERT`, turn off IP reassembly | an hour, plus the bench round | see below |
| B | **Switch now**, with one segment in flight | about a week: USB driver, adapter, DNS answer, statistics, tests | the RAM and flash; downloads possibly 10 to 100 times slower |
| C | **Give smallest_tcp a sliding-window transmit buffer first**, then switch | C's work in smallest_tcp, then B's | the RAM and flash at today's speeds |
| D | **A spike on one board.** MK1C-SD, the image with the most room: the USB driver and a minimal adapter, lwIP left in the tree | one to two days | the delayed-ACK question measured on macOS, Linux and Windows; smallest_tcp's first hardware run; real footprint figures |

### Option A in detail

lwIP's 50.8 kB of RAM is mostly one array: 24 receive buffers of 1,532
bytes, 36,771 bytes. The glue holds one received frame at a time
(`net_rx_take`), so the pool needs a handful.

| Change | Gives |
|---|---|
| `PBUF_POOL_SIZE` 24 to 6 | about 27 kB of RAM |
| `MEM_SIZE` 8,000 to 16,000 from that | N1: room for the send buffers of three streams |
| `LWIP_NOASSERT` | up to about 6 kB of flash |
| IP reassembly and fragmentation off | about 2.4 kB of flash |

Net: about 19 kB of RAM back and N1 closed, RAM-1 closed, about 8 kB of
flash, with no new code. The pool figure needs proving on the bench:
`http_stream_check.py`, `api_check.py` and `test_network.py` with
`pbuf_pool` errors at zero.

## Recommendation

1. **Do option A now.** It closes N1 and RAM-1 with configuration alone and
   does not prejudice the switch.
2. **Do option D next.** It answers the one question that decides between B
   and C, and it is the hardware run smallest_tcp needs in any case.
3. **Then C or B as D shows.** If hosts acknowledge a lone segment promptly,
   B is enough. If they delay, the sliding-window buffer comes first.

Not recommended: B without D. It risks trading a memory problem that
configuration can fix for a download speed that only stack work can.

## Spike results (2026-10-04)

Option D was built on the branch `smallest-tcp-spike` and run on the
MK1C-SD at 192.168.42.1, against a macOS host. lwIP is still the default for
every build.

**The result:** smallest_tcp 0.1.10 serves this firmware's HTTP on hardware,
passes the bench scripts, and on macOS downloads at about 90 % of lwIP's
speed. macOS acknowledges a lone segment at once, so the one-segment rule
costs little there. Linux and Windows were not available and are not
measured. The evidence supports option B, switching as it is, with that one
reservation.

### What was built

| Piece | Where | Size |
|---|---|---|
| Build option | `-DPYRO_NET_STACK=smallest_tcp`; the stack fetched at v0.1.10 (9293992), IPv4 only, `-Os` | `CMakeLists.txt` |
| USB driver, DHCP, mDNS, timers | `src/net_glue_stcp.c` in place of `net_glue.c` | 325 lines |
| HTTP transport | `src/http_transport_stcp.inc`, included by `http_server.c` in place of the lwIP adapter | 327 lines |
| Bench routes | `-DPYRO_NET_BENCH=ON`: `GET /api/net/blob?n=`, `POST /api/net/sink`, bytes with no storage behind them, for either stack | off by default |

- **The buffers are the rings.** The stack's receive and transmit operations
  are written over the exchange's own 2 kB rings. `tcp_buf_saw.c` is not
  linked. A sent byte stays in the tx ring until it is acknowledged.
- **Sixteen links, four exchanges,** as with lwIP. A link with no exchange
  advertises a zero window; given one, its window opens to the ring.
- **A frame the endpoint cannot take is queued,** in an 8 kB byte queue, so
  the driver refuses nothing in ordinary use.
- **DHCP and mDNS** are the stack's own. The DNS answer for `pyro.local` on
  port 53 is not built; macOS resolves the name by mDNS.
- **`/api/net`** keeps its keys. The pool figures are zero; the TCP states
  are numbered as lwIP's are.
- **The spike image is never committed.** Any reset returns the board to the
  image it was updated from, and an image that has served no request in two
  minutes resets itself. Both were tested.

### Throughput

Method: `curl` and `support/net_bench.py` on macOS over the board's USB network,
full speed. The bench routes on both images, so storage plays no part.
Ranges are over 3 to 5 runs.

| Measurement | lwIP | smallest_tcp |
|---|---|---|
| Download, 74 kB | 445 to 474 kB/s | 390 to 476 kB/s |
| Download, 1 MB | 528 to 531 kB/s | 457 to 480 kB/s |
| Upload, 300 kB | 652 to 668 kB/s | 536 to 577 kB/s |
| Eight downloads of 74 kB at once | 516 kB/s | 577 to 592 kB/s |
| OTA of the 296 kB image | 2.60 to 2.65 s | 2.80 s |
| Ping, average of 20 | 1.16 ms | 0.88 to 1.00 ms |

### The delayed acknowledgement

From a `tcpdump` capture of a 74 kB download on the spike, 72 data segments:

| Interval | Minimum | Median | Maximum |
|---|---|---|---|
| macOS acknowledges a lone segment | 35 µs | 62 µs | 286 µs |
| The board sends its next segment | 1.2 ms | 2.2 ms | 3.3 ms |

- macOS did not delay one acknowledgement, of full segments or short ones.
- The board's turnaround is the limit, and it is the USB link: a 1,514-byte
  frame takes over a millisecond at full speed. lwIP's segments leave at
  the same 2 ms spacing.
- The ring wraps, so segments alternate 1,460 and 588 bytes. That, not the
  host, is the 10 % against lwIP.
- **Linux and Windows are not measured.** Both are known to delay an
  acknowledgement in some cases, so this result does not carry over. The
  spike image and a host are all that measuring needs.

### Footprint

MK1C-SD images without the bench routes, from the linker maps.

| | lwIP | smallest_tcp | Difference |
|---|---|---|---|
| Update image | 295,936 B | 263,168 B | 32,768 B less |
| Stack code and constants | 48,584 B | 19,741 B | |
| TinyUSB DHCP, DNS, RNDIS helpers | 2,118 B | 680 B | |
| RAM, data and bss, whole image | 222,272 B | 183,792 B | 38,480 B less |
| RAM inside the stack | 50,761 B | 0 | |
| RAM the glue adds | 0 | 13,062 B | |

The glue's RAM is two 1,514-byte frame buffers, the 8 kB transmit queue,
and sixteen links of 124 bytes. The first estimate of 45 kB saved did not
count the queue.

### Function

| Check | Result |
|---|---|
| `support/api_check.py` | 51 of 51 |
| `support/http_stream_check.py` | 20 of 20 with sixteen links; with eight, the ninth socket was refused |
| `test/web/hw_ui_check.js`, a real browser | all passed |
| Eight fetches at once | 8 of 8 whole |
| DHCP | the host took 192.168.42.2 from the stack's server |
| mDNS | `pyro.local` resolves to the board; `_pyro._tcp` is browsable |
| OTA received by the spike image | written, answered, rebooted into the new image |
| Reboot of the spike image | back on the lwIP image |
| No request for two minutes | the image reset itself; back on the lwIP image |
| Host gate, default build | every suite passes; `prove_core0` passes on the spike image |

Not tested: a replug (no hands), suspend and resume, a name conflict on
mDNS, RNDIS (Windows), and the card's files (see below).

### Faults and findings in smallest_tcp

| # | What | Where | Effect here |
|---|---|---|---|
| 1 | `net_init` is the application's name too | `include/net.h` | Link clash. The spike renames the stack's with `-Dnet_init=stcp_net_init`. A stack-wide prefix would avoid it |
| 2 | mDNS reads the IP header from `net->rx.buf` | `src/mdns.c:1588-1594` | A driver that hands `eth_input()` its own buffer, which `mac-hal.md` allows, breaks mDNS. The spike copies each frame into `rx.buf` |
| 3 | A segment before the window is dropped whole, its acknowledgement with it | `src/tcp.c:900-905` | When both ends close at once, the peer repeats its FIN with the ACK of ours on it; the stack discards it and stays in CLOSING until its FIN times out a second later. Seen with Python's client. The spike reuses a link once it reaches CLOSING |
| 4 | No accept queue | by design | The ninth socket was refused with eight links. Sixteen links cost 2 kB and cure it |
| 5 | A frame the driver refuses is a lost segment | `src/tcp.c:386-405` | With a 4 kB queue, eight fetches at once refused 1 to 3 frames, each a 1 s wait: 1.54 s against 1.02 s. With 8 kB, none |
| 6 | `next_segment()` wants one contiguous run | `include/tcp_buf.h` | Over a ring, every wrap is a short segment. A ring-aware call, or two runs, would send full segments |

A zero window in the SYN-ACK, opened by a window update a moment later,
worked with macOS without a visible delay.

### Seen on the board, not laid to the stack

- **One spike boot of 18 reset at once** and the board returned to lwIP. The
  same was seen earlier today with lwIP images on this board.
- **One spike boot of 18 found no pressure sensor** and came up in FAULT.
  The next boots found it.
- **The card stopped answering under lwIP v2.2.5.** After one whole 709 kB
  download of `/logs/hr0013.bin`, reads failed: files answered 404, uploads
  500, and one download ended at 47 kB with status 200. `/api/sd` counted
  timeouts and 76 retries. A reboot cleared it. This is why the throughput
  figures use the bench routes.
- **lwIP's own counters in the baseline run:** the receive pool peaked at 5
  of 24 buffers, and the heap refused 3,839 allocations. Both support
  option A.

### What remains for a real port

1. Measure Linux and Windows with the spike image.
2. A DNS answer on port 53, or a ruling that mDNS is enough.
3. `/api/net`, `net_stats.c` and the scripts' checks rewritten for what
   this stack can report.
4. `rndis_reports.c` from TinyUSB still includes an lwIP header; lwIP's
   include path is kept for that one file.
5. A host test of the transport against the real stack, which the lwIP
   adapter never had.
6. The transport as its own source file, and the lwIP adapter out of
   `http_server.c`, in place of the `#if` and the included file.
7. The other boards: the RAM gained is what the Lua boards lack.
8. Replug, suspend and resume, and an mDNS conflict, on the bench.
9. Findings 1 to 3 and 6 taken up in smallest_tcp.

### Verdict

Against macOS the stop-and-wait transmit is not the obstacle this note
expected: downloads run at 87 to 90 % of lwIP's speed, uploads at 82 to
86 %, and several at once run faster. The stack ran on hardware at the
first attempt. Option B, switching as it is, is supported by what was
measured. Option C, the sliding-window buffer first, is needed only if
Linux or Windows turn out to delay their acknowledgements on this link,
which one afternoon with each host will show.

## Sources

- lwIP use and footprint: `src/net_glue.c`, `src/http_server.c:1704-2152`,
  `src/lwipopts.h`, `build/pyro_fw_mk1b.elf.map`, DD-039, DD-070,
  `docs/outstanding_tasks.md` N1 and RAM-1.
- smallest_tcp: `docs/design/tcp.md` (states, timers, section 8.1 not
  implemented), `docs/design/tcp-buffer.md` (stop-and-wait, section 5 ring
  design), `docs/design/mac-hal.md` (drivers, no USB), `docs/design/timer-model.md`,
  `docs/design/dhcpv4.md`, `docs/design/mdns.md`, `docs/test-plan.md`,
  `README.md` (sizes, coverage).
