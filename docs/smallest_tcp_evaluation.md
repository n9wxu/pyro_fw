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

Done on 2026-10-04 as DD-096, with different numbers from those proposed
below once measured: the pool went to 12, not 6 (its high-water mark was 12
with four segments of window, 8 with two), and the heap to 20,000. Heap
refusals went from 5,483 to at most 2 in the same load round; the MK1B image
is 16 kB smaller and 6.6 kB of RAM came back. The proposal as written:

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

## Sources

- lwIP use and footprint: `src/net_glue.c`, `src/http_server.c:1704-2152`,
  `src/lwipopts.h`, `build/pyro_fw_mk1b.elf.map`, DD-039, DD-070,
  `docs/outstanding_tasks.md` N1 and RAM-1.
- smallest_tcp: `docs/design/tcp.md` (states, timers, section 8.1 not
  implemented), `docs/design/tcp-buffer.md` (stop-and-wait, section 5 ring
  design), `docs/design/mac-hal.md` (drivers, no USB), `docs/design/timer-model.md`,
  `docs/design/dhcpv4.md`, `docs/design/mdns.md`, `docs/test-plan.md`,
  `README.md` (sizes, coverage).
