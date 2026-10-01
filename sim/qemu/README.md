# Running the firmware on an emulated RP2040

`boards/sim_mk1*` run the real board code on the host. This runs the real
**ARM binary** on an emulated RP2040, with the same `sim/plant/` model
attached to its GPIO and ADC.

The host path is faster and easier to debug, and it should stay the first
place a question gets asked. This path exists for the questions it cannot
answer — anything involving **two cores**: the `multicore_launch_core1_raw`
handshake, `malloc_mutex`, and the flash window of `src/flash_window.h`,
where core0 erasing flash takes XIP away from a core1 that is executing
from it. `support/prove_core0.py` checks statically which of those waits
core0 can reach. This is where they happen.

## Setup

QEMU has no RP2040 machine upstream. The machine is an out-of-tree RFC
series, and the pyro devices are out of tree on top of that, so there are
two layers of "not upstream" here and it is worth knowing which is which:

| | |
|---|---|
| `2xs/qemu-rp2040-pico` | the RP2040 machine itself, continuing Alex Bennee's 2022 RFC. Someone else's work, still an RFC on qemu-devel. |
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
`2xs` and `pyro-plant` is a single commit on top of its
`rp2040-pico-v4-validation`, so rebasing forward stays a one-commit job.

Then, after any change under `sim/plant/`:

```sh
./sim/qemu/sync-plant.sh ~/src/qemu-rp2040-pico && ninja -C ~/src/qemu-rp2040-pico/build
```

The plant is canonical in *this* repository. The copy under
`hw/misc/pyro-plant/` in the QEMU tree is generated, because QEMU's meson
cannot reach outside its own source root. The script copies each board's
`board_pins.h` but not `boards/mk1c/pyro_sense.h`, which `plant_mk1c.c`
includes (DD-054): copy it into `hw/misc/pyro-plant/mk1c/` too, or the QEMU
build fails.

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
`verbose=true` and `-D <file>` every pin edge is logged with its timestamp;
`run-qemu.sh` passes no extra QEMU arguments, so add `-D` to its last line.

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

MK1A boots FreeRTOS SMP on both cores, mounts littlefs, finds its
barometer, flies, and fires both channels:

```
pyro-plant: launch at 14.001 s, target apogee 1524 m
pyro-plant: match 1 IGNITED at 33077 ms, alt 1524.2 m
pyro-plant: match 2 IGNITED at 123612 ms, alt 300.3 m

$PYRO_APO,152530,18921        apogee 1525.30 m
$PYRO_FIRE,1,152401,18941     drogue at 1524 m
$PYRO_FIRE,2,30718,109473     main at 307 m
```

Both ignitions come from the plant's own energy integrator, not from the
fire command, so a channel that would not have lit does not deploy
anything. The loop is closed: physics to the barometer, the barometer to
the firmware, the firmware's FIRE pin to the plant, the plant's ignition
latch back to the chutes.

Continuity tracks the plant as it does on the host, and the two boards
still disagree about a healthy igniter, now on emulated silicon:

```
plant: ch1 present, ch2 absent
  mk1a   $PYRO,...,01,0,4095,0,0      flags 01 = channel 1 continuity good
  mk1b   $PYRO,...,00,0,4095,0,0      flags 00 = neither channel good
```

### One thing to look at

A flight shows `$PYRO_FIRE,1` twice, about 2 s apart, the second from the
re-fire path in `check_refire()`. That may be the re-fire logic working as
designed on a channel it believed was still intact, or the spent match not
reaching the firmware before the window opens. Not root-caused.

### Two things that bite

**Configure headless.** A desktop library that moves under the binary
breaks it with a dyld error that looks nothing like a QEMU problem. The
configure line above passes `--disable-sdl --disable-gtk --disable-vnc
--disable-curses --disable-cocoa`; nothing here ever opens a window.

**The watchdog can reset the guest.** Without `-icount`, virtual time
follows host wall-clock, so the emulated core does less work per virtual
millisecond than real silicon and a watchdog sized for real hardware can
fire. It shows up as a clean QEMU exit under `-no-reboot` — a guest reset,
not a crash, with nothing in the log. `-icount shift=3` should fix it by
tying virtual time to instructions at 125 MHz, but produced no output at
all on this machine and was not pursued; a longer `PYRO_LOOP_WORST_MS` for
emulated runs is the cruder option.

## TODO

### 1. Lua on core 1

**The framing here changed.** This item used to be about the bare-metal
core0/core1 split and `src/flash_window.h` — core0 erasing flash while
core1 executed from XIP. That file is gone; FreeRTOS SMP replaced it. What
is left of the original list:

- `multicore_launch_core1_raw()`'s unbounded handshake: **exercised**, and
  it found three real emulator bugs (see the commit above). The FreeRTOS
  port calls it on every boot, so every boot now exercises it.
- `malloc_mutex` from linking `pico_multicore` (`docs/core1_hazard.md`):
  still worth driving, and now reachable, since the scheduler runs.
- the flash window: no longer applicable in this form. The equivalent
  question under SMP is what happens to a task on the other core during a
  flash erase, and the RFC does model that — its XIP is ROMD-backed
  normally and switches to device callbacks while flash is busy, with NOR
  programming rules and busy faults.

Lua itself still does not run: `lua_core1_start()` needs a script, and the
emulated littlefs is empty. The way in is a pre-built littlefs image laid
into the flash array at the filesystem offset, so a script is present at
boot without needing the web interface (item 2 — do not wait for it).
littlefs is vendored under `_deps/littlefs-src`, so a small host writer can
build the image; `littlefs-python` is the other option.

### 2. Web interface — needs a USB network bridge

The firmware serves its web UI over lwIP on USB ECM (TinyUSB device stack).
The RFC machine has "USB DPRAM and shallow USB controller register storage"
and explicitly no "device and host transactions, endpoint state machines".
So the guest never enumerates, and every frame lwIP sends is refused as not
ready (`NET_TX_NOT_READY`, `src/net_txq.h`).

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

Worth weighing against the cheaper option first: `http_tests` already runs
`src/http_conn.c` against a fake transport, and `http_work_tests` runs
`src/http_work.c`, which catches most web bugs for a fraction of the effort.
They do **not** exercise the interaction the flash window is about — HTTP
uploads landing on flash writes, with core1 running HTTP units (DD-061) —
and that interaction is the reason to want the real thing.

### 3. Pressure

No pressure sensor model, so bring-up ends in `!PRES init FAIL`, the board
reports `!SENSOR FAIL` and stays in FAULT, and flights never start. A BMP280
(MK1A, MK1B) or MS5607 (MK1B, MK1C) on the new `rp2040-i2c` bus, fed by
`sim/physics.c`, would give full flights on emulated hardware — and the I2C
controller already drives a real `I2CBus`, so the device can just be
attached. The bring-up's transfers are the SDK's `_timeout_us` forms
(DD-069), so the model must answer within their bounds.

### 4. MK1C

Needs PIO for the ARM_TOGGLE charge pump, which the RFC does not implement
at all. MK1A and MK1B need no PIO; MK1C is a bigger piece of work than the
other two put together.
