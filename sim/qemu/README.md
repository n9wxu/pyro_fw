# Running the firmware on an emulated RP2040

`boards/sim_mk1*` run the real board code on the host. This runs the real
**ARM binary** on an emulated RP2040, with the same `sim/plant/` model
attached to its GPIO and ADC.

The host path is faster and easier to debug, and it should stay the first
place a question gets asked. This path exists for the questions it cannot
answer — anything involving **two cores**: the `multicore_launch_core1_raw`
handshake the FreeRTOS SMP port makes at every boot, `malloc_mutex`, and a
flash operation, which takes XIP away from both cores and parks the other
core in RAM while it runs (DD-074, `src/flash_op.h`).
`support/prove_core0.py` checks statically which of those waits core0 can
reach. This is where they happen.

## Setup

QEMU has no RP2040 machine upstream. The machine is an out-of-tree RFC
series, and the pyro devices are out of tree on top of that, so there are
two layers of "not upstream" here and it is worth knowing which is which:

| | |
|---|---|
| `2xs/qemu-rp2040-pico` | the RP2040 machine itself, continuing Alex Bennee's 2022 RFC. Someone else's work, an RFC on qemu-devel. |
| `n9wxu/qemu-rp2040-pico`, branch `pyro-plant` | that, plus the four pyro devices below. Ours. |

```sh
git clone -b pyro-plant https://github.com/n9wxu/qemu-rp2040-pico.git ~/src/qemu-rp2040-pico
cd ~/src/qemu-rp2040-pico
mkdir build && cd build
../configure --target-list=arm-softmmu --disable-werror --disable-docs \
             --disable-tools --disable-guest-agent --disable-capstone \
             --disable-slirp --disable-sdl --disable-gtk --disable-vnc \
             --disable-curses --disable-cocoa
ninja
```

The build takes a couple of minutes and needs the usual QEMU dependencies;
on macOS that is `brew install ninja pkg-config glib pixman` (meson is
fetched into QEMU's own venv by `configure`).

To follow the RFC as it is rebased, the fork keeps `upstream` pointing at
`2xs` and `pyro-plant` is two commits on top of its
`rp2040-pico-v4-validation`, so rebasing forward stays small.

Then, after any change under `sim/plant/`:

```sh
./sim/qemu/sync-plant.sh ~/src/qemu-rp2040-pico && ninja -C ~/src/qemu-rp2040-pico/build
```

The plant is canonical in *this* repository. The copy under
`hw/misc/pyro-plant/` in the QEMU tree is generated, because QEMU's meson
cannot reach outside its own source root. The script copies the plant, each
board's `board_pins.h`, MK1C's `boards/mk1c/pyro_sense.h` (DD-054) and
`sim/physics.c`, which the device flies; it fails if a copied file includes a
header it did not copy.

## Use

```sh
cmake -B build-mk1a -DPYRO_BOARD=mk1a -DCMAKE_BUILD_TYPE=Release
cmake --build build-mk1a -j8

./sim/qemu/run-qemu.sh build-mk1a
./sim/qemu/run-qemu.sh build-mk1a match1=absent,match2=present
./sim/qemu/run-qemu.sh build-mk1a match1=present,match-mohm=10000
```

`PYRO_QEMU` overrides the QEMU build directory (default
`~/src/qemu-rp2040-pico/build`). The script assembles the
bootloader and the application into one raw flash array at their own
offsets, because `-kernel` wants boot2 at `0x10000000` and the application
is linked above `pico_fota_bootloader`.

The plant device takes `board`, `match1`, `match2` (`present` / `absent` /
`short` / `spent`), `pack-mv`, `match-mohm`, `verbose`, `flight-alt-m` (the
apogee flown to after `pad-dwell-ms`, 20 s by default; 0 keeps the board on
the pad) and `baro` (`auto`, `i2c0`, `i2c1` or `off`). Options given replace
`run-qemu.sh`'s default, `match1=present,match2=present,flight-alt-m=1524`,
so a flight needs `flight-alt-m` among them. With `verbose=true` and
`-D <file>` every pin edge is logged with its timestamp; `run-qemu.sh` passes
no extra QEMU arguments, so add `-D` to its last line.

## What the `pyro-plant` branch adds

Four devices on top of the RFC machine (`b33e355`), plus three fixes to
the machine's own multicore path and a BMP280 (`415ed4d`):

| | |
|---|---|
| `hw/i2c/rp2040_i2c.c` | DW_apb_i2c master on a real QEMU `I2CBus`. Without it `i2c_write_blocking()` waits on `IC_RAW_INTR_STAT.TX_EMPTY` forever, because an unimplemented register window reads as zero. |
| `hw/adc/rp2040_adc.c` | One-shot and free-running paths, values from a board-supplied callback. |
| `hw/misc/pyro_plant.c` | The `pyro-plant` device: wires the SIO pins in, supplies ADC samples, integrates on the virtual clock. |
| `hw/misc/rp2040_sio.c` | Named `gpio-out` lines, edges only. Closes the RFC's "external GPIO signal transport is not implemented" gap. Also the ROM's core 1 entry handshake, and an event line per core so a WFE wait on the mailbox can end. |
| `hw/sensor/bmp280.c` | A barometer. It bisects Bosch's compensation to produce the raw value that maps to a chosen pressure, rather than inverting it by algebra and drifting from what the driver computes. |

Plus: a 16 MiB flash array by default, the whole XIP window, with a
`flash-size` machine property. A board keeps its files at the top of its
chip, and an array shorter than the image's flash map faults inside the
mount's scan — which surfaces as a lockup in the fault handler with nothing
pointing at the flash.

**Watch for the atomic aliases.** RP2040 peripherals alias at +0x1000 /
+0x2000 / +0x3000 for XOR / set / clear, and the SDK reaches them almost
exclusively that way: `adc_read()` is `hw_set_bits(&adc_hw->cs, START_ONCE)`
and never touches the plain window. A device that decodes only its base
address silently returns zero for everything. Use
`rp2040_atomic_update()` from `include/hw/misc/rp2040.h`.

## What works

The machine runs the bootloader, which hands over to the application. The
device closes the loop: physics to the barometer, the barometer to the
firmware, the firmware's FIRE pin to the plant, the plant's ignition latch
back to the chutes. An ignition comes from the plant's own energy
integrator, not from the fire command, so a channel that would not have lit
deploys nothing.

Measured 2026-10-20, the fork at `415ed4d`, an MK1A image built 2026-10-19,
`./sim/qemu/run-qemu.sh build-mk1a`:

```
[BOOTLOADER] End of execution, executing the application...
pyro-plant: launch at 20.001 s, target apogee 1524 m
```

and then nothing on UART0, and no match lit, in eight minutes.

### One thing to look at

Where the application stops after the bootloader hands over: nothing reaches
UART0, not even the sensor bring-up's `!PRES init` line.

### Two things that bite

**Configure headless.** A desktop library that moves under the binary
breaks it with a dyld error that looks nothing like a QEMU problem. The
configure line above passes `--disable-sdl --disable-gtk --disable-vnc
--disable-curses --disable-cocoa`; nothing here ever opens a window.

**The watchdog can reset the guest.** Without `-icount`, virtual time
follows host wall-clock, so the emulated core does less work per virtual
millisecond than real silicon and a watchdog sized for real hardware can
fire. It shows up as a clean QEMU exit under `-no-reboot` — a guest reset,
not a crash, with nothing in the log. `-icount shift=3` ties virtual time to
instructions at 125 MHz, but with it the guest prints nothing on the
author's Mac; a longer `PYRO_LOOP_WORST_MS` for emulated runs is the cruder
option.

## TODO

### 1. Lua on core 1

The two-core questions, as they stand on the emulator:

- `multicore_launch_core1_raw()`'s unbounded handshake: the FreeRTOS port
  calls it at every boot, and the fork's multicore fixes (`415ed4d`) let it
  complete.
- `malloc_mutex` from linking `pico_multicore` (`docs/core1_hazard.md`):
  reachable, since the scheduler runs, and worth driving.
- a flash operation under SMP: what a task on the other core does while one
  core erases (DD-074). The RFC models it — its XIP is ROMD-backed normally
  and switches to device callbacks while flash is busy, with NOR
  programming rules and busy faults.

Lua does not run: `lua_core1_start()` needs a script, and the emulated
flash holds no files. The way in is an image of the files laid into the
flash array at the filesystem offset, so a script is present at boot
without the web interface (item 2 — do not wait for it). alfs is under
`_deps/alfs-src` and runs on the host, so a small host writer can build the
image.

### 2. Web interface — needs a USB network bridge

The firmware serves its web UI over smallest_tcp on USB ECM (TinyUSB device stack).
The RFC machine has "USB DPRAM and shallow USB controller register storage"
and explicitly no "device and host transactions, endpoint state machines".
So the guest never enumerates, and every frame the stack sends is refused as not
ready (`NET_TX_NOT_READY`, `src/net_txq.h`).

Closing it means a **USB device-mode controller that terminates ECM inside
the device model** and passes Ethernet frames to a QEMU `-netdev`:

```
  guest stack <->  TinyUSB  <->  rp2040_usb device model
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

Worth weighing against the cheaper option first: `http_tests` runs
`src/http_conn.c` against a fake transport, `http_transport_tests` runs it
over smallest_tcp at both ends (DD-102), and `http_work_tests` runs
`src/http_work.c`; together they catch most web bugs for a fraction of the
effort. They do **not** exercise HTTP uploads landing on flash operations
that park the other core (DD-074), and that interaction is the reason to
want the real thing.

### 3. Pressure

The plant device fits a BMP280 at 0x76 on MK1A's `i2c0` and MK1B's `i2c1`,
its pressure from `sim/physics.c`'s flight. There is no MS5607 model, so an
MK1C image's bring-up ends in `!PRES init FAIL`, the board reports
`!SENSOR FAIL` and stays in FAULT, and flights never start. An MS5607 on the
`rp2040-i2c` bus, fed the same way, is what MK1C needs beside PIO (item 4);
the I2C controller drives a real `I2CBus`, so the device can just be
attached. The bring-up's transfers are the SDK's `_timeout_us` forms
(DD-069), so the model must answer within their bounds.

### 4. MK1C

Needs PIO for the ARM_TOGGLE charge pump, which the RFC does not implement
at all. MK1A and MK1B need no PIO; MK1C is a bigger piece of work than the
other two put together.
