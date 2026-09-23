# Running the firmware on an emulated RP2040

`boards/sim_mk1*` run the real board code on the host. This runs the real
**ARM binary** on an emulated RP2040, with the same `sim/plant/` model
attached to its GPIO and ADC.

The host path is faster and easier to debug, and it should stay the first
place a question gets asked. This path exists for the questions it cannot
answer — anything involving **two cores**: the `multicore_launch_core1_raw`
handshake, `malloc_mutex`, and the flash window of `src/flash_window.h`,
where core0 erasing flash takes XIP away from a core1 that is executing
from it. `support/prove_core0.py` proves statically that those hazards
*can* happen. This is where they happen.

## Setup

QEMU has no RP2040 machine upstream; the work is an out-of-tree RFC series.

```sh
git clone https://github.com/2xs/qemu-rp2040-pico.git ~/src/qemu-rp2040-pico
cd ~/src/qemu-rp2040-pico
# apply the pyro device patches (see "What the fork needs" below)
mkdir build && cd build
../configure --target-list=arm-softmmu --disable-werror --disable-docs \
             --disable-tools --disable-guest-agent --disable-capstone --disable-slirp
ninja
```

Then, after any change under `sim/plant/`:

```sh
./sim/qemu/sync-plant.sh ~/src/qemu-rp2040-pico && ninja -C ~/src/qemu-rp2040-pico/build
```

The plant is canonical in *this* repository. The copy under
`hw/misc/pyro-plant/` in the QEMU tree is generated, because QEMU's meson
cannot reach outside its own source root.

## Use

```sh
cmake -B build-mk1a -DPYRO_BOARD=mk1a -DCMAKE_BUILD_TYPE=Release
cmake --build build-mk1a -j8

./sim/qemu/run-qemu.sh build-mk1a
./sim/qemu/run-qemu.sh build-mk1a match1=absent,match2=present
./sim/qemu/run-qemu.sh build-mk1a match1=present,match-mohm=10000
```

`PYRO_QEMU` overrides the QEMU build directory. The script assembles the
bootloader and the application into one raw flash array at their own
offsets, because `-kernel` wants boot2 at `0x10000000` and the application
is linked above `pico_fota_bootloader`.

The plant device takes `board`, `match1`, `match2` (`present` / `absent` /
`short` / `spent`), `pack-mv`, `match-mohm` and `verbose`. With
`verbose=true` and `-D <file>` every pin edge is logged with its timestamp,
which is how the sense cadence below was read off.

## What the fork needs

Four changes, none upstream:

| | |
|---|---|
| `hw/i2c/rp2040_i2c.c` | DW_apb_i2c master on a real QEMU `I2CBus`. Without it `i2c_write_blocking()` waits on `IC_RAW_INTR_STAT.TX_EMPTY` forever, because an unimplemented register window reads as zero. |
| `hw/adc/rp2040_adc.c` | One-shot and free-running paths, values from a board-supplied callback. |
| `hw/misc/pyro_plant.c` | The `pyro-plant` device: wires the SIO pins in, supplies ADC samples, integrates on the virtual clock. |
| `hw/misc/rp2040_sio.c` | Named `gpio-out` lines, edges only. Closes the RFC's "external GPIO signal transport is not implemented" gap. |

Plus: the machine's flash array default raised to 16 MiB with a `flash-size`
machine property, since MK1A puts littlefs at the 8 MB mark and a short
array faults inside `lfs_mount()` — which surfaces as a lockup in the fault
handler with nothing pointing at the flash.

**Watch for the atomic aliases.** RP2040 peripherals alias at +0x1000 /
+0x2000 / +0x3000 for XOR / set / clear, and the SDK reaches them almost
exclusively that way: `adc_read()` is `hw_set_bits(&adc_hw->cs, START_ONCE)`
and never touches the plain window. A device that decodes only its base
address silently returns zero for everything. Use
`rp2040_atomic_update()` from `include/hw/misc/rp2040.h`.

## What works

MK1A and MK1B boot through the bootloader, mount littlefs, probe I2C, reach
the main loop and emit telemetry that tracks the plant:

```
plant: ch1 present, ch2 absent
  mk1a   $PYRO,40,...,01,0,4095,0,0      flags 01 = channel 1 continuity good
  mk1b   $PYRO,40,...,00,0,4095,0,0      flags 00 = neither channel good
```

Same plant, same raw counts, opposite verdicts — MK1B classifies a healthy
1 ohm igniter as `shorted`. The sense cadence is real, not scripted:

```
GPIO10 -> 1 at 2062.373 ms     PYRO_LOW asserted
GPIO10 -> 0 at 2112.761 ms     +50 ms, SETTLE_MS
GPIO10 -> 1 at 2562.743 ms     +450 ms, IDLE_MS
```

## TODO

### 1. Lua on core1 — the reason this path exists

Core1 never launches today: `lua_core1_start()` is only called when there is
a script to run, and the emulated littlefs is empty. Nothing about the
multicore hazard surface is being exercised yet.

The way in is a pre-built littlefs image laid into the flash array at the
filesystem offset, so a script is present at boot without needing the web
interface (which needs networking, which is item 2 — do not wait for it).
littlefs is already vendored under `_deps/littlefs-src`, so a small host
writer can build the image; `littlefs-python` is the other option.

Once a script runs, the things worth driving:

- the `multicore_launch_core1_raw()` push/pop handshake, which has no
  timeout — `lua_core1.c` already works around a stray-0 case
- `malloc_mutex`, which appears purely from linking `pico_multicore`
  (`docs/core1_hazard.md`)
- the flash window: core0 erasing while core1 executes from XIP. The RFC
  models this — its XIP is ROMD-backed normally and switches to device
  callbacks while flash is busy, with NOR programming rules and busy
  faults — so the fault is real rather than elided
- `flash_window_skips` / `refusals` / `deferrals` under a core1 that
  overruns its grant

Run with `-icount 0` for a reproducible instruction-accurate clock; race
hunting is not worth much without it.

### 2. Web interface — needs a USB network bridge

The firmware serves its web UI over lwIP on USB ECM (TinyUSB device stack).
The RFC machine has "USB DPRAM and shallow USB controller register storage"
and explicitly no "device and host transactions, endpoint state machines".
So today the guest never enumerates and the log fills with
`!NET tx fail (not ready)`.

Closing it means a **USB device-mode controller that terminates ECM inside
the device model** and passes Ethernet frames to a QEMU `-netdev`:

```
  guest lwIP  <->  TinyUSB  <->  rp2040_usb device model
                                     | ECM terminated here
                                 qemu_send_packet / receive
                                     |
                                 -netdev user,hostfwd=tcp::8080-:80
```

QEMU's own USB framework is host-side, so `usb-net` is the wrong direction
and cannot be reused directly. What is needed is the RP2040 device
controller: DPRAM buffer-control registers, EP0 SETUP handling, and the
endpoint state machines — call it a few days, most of it spent on
enumeration.

Worth weighing against the cheaper option first: `src/http_server.c` and
the `www/` handlers can be linked host-side against a socket shim, which
catches most web bugs for a fraction of the effort. It does **not** exercise
the interaction that `DECISIONS.md` #2 and the flash window are about —
HTTP uploads landing on flash writes — and that interaction is the reason
to want the real thing.

### 3. Pressure

No BMP280 model, so `!PRES init FAIL` and `!CAL TIMEOUT` force PAD_IDLE and
flights never progress. A BMP280 on the new `rp2040-i2c` bus, fed by
`sim/physics.c`, would give full flights on emulated hardware — and the
I2C controller already drives a real `I2CBus`, so the device can just be
attached.

### 4. MK1C

Needs PIO for the ARM_TOGGLE charge pump, which the RFC does not implement
at all. MK1A and MK1B need no PIO; MK1C is a bigger piece of work than the
other two put together.
