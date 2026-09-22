/*
 * Lua platform for RP2040 boards — shared implementation.
 *
 * Board-independent. Everything that differs between boards is the pin list,
 * which each board states in its own lua_pins.h:
 *
 *      boards/mk1a/lua_pins.h   J6   GPIO18, GPIO19
 *      boards/mk1b/lua_pins.h   J1   GPIO8
 *      boards/mk1c/lua_pins.h   J3   GPIO18-21
 *
 * Three rules shape this file.
 *
 * 1. A Lua pin is only ever SIO or a PIO function, never a peripheral one.
 *    That is not stylistic: on RP2040 a pin's peripheral function is fixed by
 *    pin number, and on every one of these boards at least one Lua-reachable
 *    pin shares an I2C instance with the flight pressure sensor --
 *    GPIO18/19 are i2c1 on MK1C, whose MS5607 is on GPIO6/7. A Lua pin in
 *    GPIO_FUNC_I2C would join the flight sensor's bus. There is no call to
 *    gpio_set_function with a peripheral argument anywhere below, which
 *    removes the reachability instead of checking for it.
 *
 * 2. Every PIO state machine, program offset and DMA channel is claimed here,
 *    at boot, on core0, before core1 exists. hw_claim_lock() takes spin lock
 *    11; a core1 killed inside it would strand that lock and hang core0's next
 *    claim. Claiming everything up front means core1 never calls hw_claim at
 *    all -- it only writes registers on resources it was handed.
 *
 * 3. Nothing here blocks. core1 must stay able to answer a park request, so a
 *    full FIFO drops rather than waits and a busy DMA skips a frame.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_platform.h"
#include "board_pins.h"
#include "lua_pins.h"
#include "lua_pio.pio.h"
#include "lua_core1.h"
#include "lua_platform_cfg.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include <string.h>

/* ── Resource budget ──────────────────────────────────────────────
 *
 * The point of this block is that acquisition failure is not a runtime
 * outcome to be handled, it is a state the build refuses to produce.
 *
 * pio1 has 32 instruction slots and 4 state machines. J3 has four pins, and
 * a pin holds exactly one role. The worst case an operator can configure is
 * therefore all four pins in the three PIO-backed roles:
 *
 *      ws2812   4 instructions   1 SM
 *      uart_tx  4 instructions   1 SM
 *      uart_rx  8 instructions   1 SM
 *      ------------------------------
 *               16 instructions  3 SMs      (out of 32 and 4)
 *
 * Digital outputs and inputs are plain SIO and cost neither. So every
 * configuration fits, with the fourth SM and half the instruction memory to
 * spare, and the static_asserts below fail the BUILD if a program ever grows
 * past that -- which is the point. An operator cannot produce a
 * configuration this firmware will refuse to start.
 *
 * Counted from the generated instruction arrays rather than the pio_program
 * structs, because a struct member is not a constant expression and a
 * static_assert that cannot see the number is not a check. */
#define LUA_PIO_PROG_LEN(p) (sizeof(p##_program_instructions) / sizeof(uint16_t))
#define LUA_PIO_BUDGET_INSTR                                                                                           \
    (LUA_PIO_PROG_LEN(lua_ws2812) + LUA_PIO_PROG_LEN(lua_uart_tx) + LUA_PIO_PROG_LEN(lua_uart_rx))
#define LUA_PIO_BUDGET_SMS 3

/* Which PIO block and which pads -- the board says, because the board is the
 * only thing that knows. See boards/<name>/lua_pins.h. */
#define LUA_PIO LUA_PIO_INST
static const uint8_t lua_pins[LUA_PIN_COUNT] = LUA_PIN_LIST;

/* ── Configuration, resolved once at boot ─────────────────────────
 *
 * lua_plat_configure() turns the operator's config into these tables. Lua
 * addresses them by index through name lookup in pyro_lua.c, so a resource
 * that configuration did not create cannot be named, let alone reached. */

#define LUA_MAX_OUT LUA_PIN_COUNT
#define LUA_MAX_IN LUA_PIN_COUNT
#define LUA_MAX_SERIAL 1
#define LUA_MAX_PIXELS 256

typedef struct {
    lua_output_desc_t desc;
    char name[LUA_NAME_MAX];
    uint8_t pin;
    int value;
} out_t;

typedef struct {
    lua_input_desc_t desc;
    char name[LUA_NAME_MAX];
    uint8_t pin;
} in_t;

static out_t outs[LUA_MAX_OUT];
static int n_out;
static in_t ins[LUA_MAX_IN];
static int n_in;

static lua_serial_desc_t serial_desc;
static char serial_name[LUA_NAME_MAX];
static int n_serial;
static int tx_sm = -1, rx_sm = -1;

static int px_count;
static int px_sm = -1;
static int px_dma = -1;
static uint32_t px_buf[LUA_MAX_PIXELS];  /* GRB<<8, the wire format */
static uint32_t px_wire[LUA_MAX_PIXELS]; /* what show() handed to DMA */

/* ── PWM by software, on core1 ────────────────────────────────────
 *
 * Dimmable outputs use a 256-step software PWM driven from
 * lua_plat_pin_service(), which core1 calls between VM slices. The hardware
 * PWM slices are deliberately not used: a PWM slice keeps running after the
 * processor that set it stops, and "the output stops when the program stops"
 * is a property worth keeping for anything wired into an av-bay. It is the
 * same argument that shaped the ARM_TOGGLE pump. */
static uint8_t pwm_phase;

void lua_plat_pin_service(void) {
    pwm_phase++;
    for (int i = 0; i < n_out; i++) {
        if (!outs[i].desc.dimmable) {
            continue;
        }
        int v = outs[i].value;
        gpio_put(outs[i].pin, (v > 0) && (pwm_phase < (uint8_t)v));
    }
}

/* ── Boot-time configuration ──────────────────────────────────────── */

static uint px_offset, tx_offset, rx_offset;

int lua_plat_configure(const lua_pin_cfg_t *cfg, int n, unsigned baud, int pixels) {
    static_assert(LUA_PIO_BUDGET_INSTR <= 32, "Lua PIO programs exceed pio1 instruction memory");
    static_assert(LUA_PIO_BUDGET_SMS <= 4, "Lua PIO roles exceed pio1 state machines");

    n_out = n_in = n_serial = px_count = 0;
    tx_sm = rx_sm = px_sm = px_dma = -1;

    bool want_px = false, want_tx = false, want_rx = false;
    for (int i = 0; i < n && i < LUA_PIN_COUNT; i++) {
        switch (cfg[i].role) {
        case LUA_ROLE_PIXEL:
            want_px = true;
            break;
        case LUA_ROLE_TX:
            want_tx = true;
            break;
        case LUA_ROLE_RX:
            want_rx = true;
            break;
        default:
            break;
        }
    }

    /* Claim first, wire second. Every claim below is on core0 at boot, and
     * by the budget above none of them can fail; the SDK's asserting
     * variants would be the wrong tool anyway, so the non-asserting ones are
     * used and a failure is reported rather than panicking. */
    if (want_px) {
        px_sm = pio_claim_unused_sm(LUA_PIO, false);
        px_dma = dma_claim_unused_channel(false);
        if (px_sm < 0 || px_dma < 0 || !pio_can_add_program(LUA_PIO, &lua_ws2812_program)) {
            return -1;
        }
        px_offset = pio_add_program(LUA_PIO, &lua_ws2812_program);
    }
    if (want_tx) {
        tx_sm = pio_claim_unused_sm(LUA_PIO, false);
        if (tx_sm < 0 || !pio_can_add_program(LUA_PIO, &lua_uart_tx_program)) {
            return -1;
        }
        tx_offset = pio_add_program(LUA_PIO, &lua_uart_tx_program);
    }
    if (want_rx) {
        rx_sm = pio_claim_unused_sm(LUA_PIO, false);
        if (rx_sm < 0 || !pio_can_add_program(LUA_PIO, &lua_uart_rx_program)) {
            return -1;
        }
        rx_offset = pio_add_program(LUA_PIO, &lua_uart_rx_program);
    }

    for (int i = 0; i < n && i < LUA_PIN_COUNT; i++) {
        uint8_t pin = lua_pins[i];
        switch (cfg[i].role) {
        case LUA_ROLE_OUT:
        case LUA_ROLE_PWM:
            if (n_out < LUA_MAX_OUT) {
                gpio_init(pin);
                gpio_set_dir(pin, GPIO_OUT);
                gpio_put(pin, 0);
                strncpy(outs[n_out].name, cfg[i].name, LUA_NAME_MAX - 1);
                outs[n_out].desc.name = outs[n_out].name;
                outs[n_out].desc.dimmable = (cfg[i].role == LUA_ROLE_PWM);
                outs[n_out].pin = pin;
                outs[n_out].value = 0;
                n_out++;
            }
            break;
        case LUA_ROLE_IN:
            if (n_in < LUA_MAX_IN) {
                gpio_init(pin);
                gpio_set_dir(pin, GPIO_IN);
                gpio_pull_down(pin);
                strncpy(ins[n_in].name, cfg[i].name, LUA_NAME_MAX - 1);
                ins[n_in].desc.name = ins[n_in].name;
                ins[n_in].pin = pin;
                n_in++;
            }
            break;
        case LUA_ROLE_TX:
            lua_uart_tx_program_init(LUA_PIO, (uint)tx_sm, tx_offset, pin, baud);
            if (n_serial == 0) {
                strncpy(serial_name, cfg[i].name, LUA_NAME_MAX - 1);
                serial_desc.name = serial_name;
                n_serial = 1;
            }
            break;
        case LUA_ROLE_RX:
            lua_uart_rx_program_init(LUA_PIO, (uint)rx_sm, rx_offset, pin, baud);
            if (n_serial == 0) {
                strncpy(serial_name, cfg[i].name, LUA_NAME_MAX - 1);
                serial_desc.name = serial_name;
                n_serial = 1;
            }
            break;
        case LUA_ROLE_PIXEL:
            lua_ws2812_program_init(LUA_PIO, (uint)px_sm, px_offset, pin);
            px_count = (pixels > LUA_MAX_PIXELS) ? LUA_MAX_PIXELS : pixels;
            {
                dma_channel_config dc = dma_channel_get_default_config((uint)px_dma);
                channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
                channel_config_set_read_increment(&dc, true);
                channel_config_set_write_increment(&dc, false);
                channel_config_set_dreq(&dc, pio_get_dreq(LUA_PIO, (uint)px_sm, true));
                dma_channel_configure((uint)px_dma, &dc, &LUA_PIO->txf[px_sm], px_wire, (uint)px_count, false);
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

/* ── Outputs ──────────────────────────────────────────────────────── */

int lua_plat_pin_count(void) {
    return LUA_PIN_COUNT;
}

int lua_plat_output_count(void) {
    return n_out;
}
const lua_output_desc_t *lua_plat_output_desc(int idx) {
    return &outs[idx].desc;
}
void lua_plat_output_set(int idx, int value) {
    outs[idx].value = value;
    if (!outs[idx].desc.dimmable) {
        gpio_put(outs[idx].pin, value > 0);
    }
    /* dimmable pins are driven by lua_plat_pin_service() */
}
int lua_plat_output_get(int idx) {
    return outs[idx].value;
}

/* ── Inputs ───────────────────────────────────────────────────────── */

int lua_plat_input_count(void) {
    return n_in;
}
const lua_input_desc_t *lua_plat_input_desc(int idx) {
    return &ins[idx].desc;
}
int lua_plat_input_get(int idx) {
    return gpio_get(ins[idx].pin) ? 1 : 0;
}

/* ── Serial ───────────────────────────────────────────────────────── */

int lua_plat_serial_count(void) {
    return n_serial;
}
const lua_serial_desc_t *lua_plat_serial_desc(int idx) {
    (void)idx;
    return &serial_desc;
}

int lua_plat_serial_write(int idx, const char *s, int len) {
    (void)idx;
    if (tx_sm < 0) {
        return 0;
    }
    /* Drop rather than block when the FIFO is full. A script that outruns
     * 9600 baud must not be able to stall the core it runs on, and a stalled
     * core1 is a core1 that cannot answer a park request. */
    int n = 0;
    while (n < len && !pio_sm_is_tx_fifo_full(LUA_PIO, (uint)tx_sm)) {
        pio_sm_put(LUA_PIO, (uint)tx_sm, (uint32_t)(uint8_t)s[n]);
        n++;
    }
    return n;
}

int lua_plat_serial_read(int idx, char *buf, int max) {
    (void)idx;
    if (rx_sm < 0) {
        return 0;
    }
    int n = 0;
    while (n < max && !pio_sm_is_rx_fifo_empty(LUA_PIO, (uint)rx_sm)) {
        /* The RX program shifts right into a 32-bit register, so the byte
         * lands in the top 8 bits. */
        buf[n++] = (char)(pio_sm_get(LUA_PIO, (uint)rx_sm) >> 24);
    }
    return n;
}

/* ── Pixels ───────────────────────────────────────────────────────── */

int lua_plat_pixel_count(void) {
    return px_count;
}

void lua_plat_pixel_set(int idx, uint8_t r, uint8_t g, uint8_t b) {
    /* WS2812 wants GRB, MSB first, and the SM autopulls 24 bits from the top
     * of the word. */
    px_buf[idx] = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
}

void lua_plat_pixel_show(void) {
    if (px_sm < 0 || px_count == 0) {
        return;
    }
    /* Drop the frame if the previous transfer has not finished.
     *
     * This used to be dma_channel_wait_for_finish_blocking(), justified as
     * "bounded by 30 us per pixel". That reasoning holds only while the state
     * machine keeps draining the FIFO; if it ever stops, the wait is
     * unbounded -- and on the bench it was. core1 froze inside it, stopped
     * answering park requests, and core0 killed it (parks ok=1, fail req=3
     * ack=1).
     *
     * The rule this file opens with says nothing here blocks, and an
     * unbounded wait on core1 is exactly what prove_core0.py now refuses.
     * A dropped frame on an LED string costs nothing; the next show() sends
     * the current buffer anyway. */
    if (dma_channel_is_busy((uint)px_dma)) {
        return;
    }
    memcpy(px_wire, px_buf, sizeof(uint32_t) * (size_t)px_count);
    dma_channel_set_read_addr((uint)px_dma, px_wire, false);
    dma_channel_set_trans_count((uint)px_dma, (uint32_t)px_count, true);
}

/* ── Flight state (read-only; see invariant L11) ──────────────────── */

int32_t lua_plat_pressure_pa(void) {
    return lua_flight_snapshot()->pressure_pa;
}
int32_t lua_plat_altitude_cm(void) {
    return lua_flight_snapshot()->altitude_cm;
}
int32_t lua_plat_speed_cms(void) {
    return lua_flight_snapshot()->speed_cms;
}
int32_t lua_plat_max_altitude_cm(void) {
    return lua_flight_snapshot()->max_alt_cm;
}
int lua_plat_flight_state(void) {
    return lua_flight_snapshot()->state;
}
uint32_t lua_plat_time_ms(void) {
    return lua_flight_snapshot()->time_ms;
}
int lua_plat_pyro_status(int channel) {
    return lua_flight_snapshot()->pyro[(channel == 2) ? 1 : 0];
}
/* Free-running microseconds. One timer register read, no lock, safe from
 * either core -- which matters because this is called from the VM's
 * instruction hook on core1. */
uint32_t lua_plat_now_us(void) {
    return time_us_32();
}

int lua_plat_pyro_adc(int channel) {
    return lua_flight_snapshot()->pyro_adc[(channel == 2) ? 1 : 0];
}
int lua_plat_under_thrust(void) {
    return lua_flight_snapshot()->under_thrust;
}
int lua_plat_apogee_detected(void) {
    return lua_flight_snapshot()->apogee_detected;
}
uint32_t lua_plat_telem_seq(void) {
    return lua_flight_snapshot()->telem_seq;
}

/* ── Safing, from core0 ───────────────────────────────────────────── */

void lua_plat_safe_outputs(void) {
    /* The pixel DMA is deliberately NOT aborted.
     *
     * dma_channel_abort() spins until the abort bit self-clears, and an
     * in-flight transfer into a PIO TX FIFO cannot complete while that FIFO is
     * full and the state machine has stopped draining it -- which is exactly
     * the state a wedged core1 leaves behind. That would put an unbounded wait
     * on core0, on the path whose whole purpose is that core0 never waits for
     * core1.
     *
     * It is also unnecessary. The safety property here is "no pad is driven",
     * and that is achieved below by handing every pad back to SIO: once the
     * function select is SIO, neither PIO nor its DMA reaches a pin, whatever
     * they go on doing. A pending transfer on a channel nothing will claim
     * again costs nothing -- there is deliberately no relaunch of core1.
     *
     * Disabling the state machines is belt and braces for the same reason. */
    if (px_sm >= 0) {
        pio_sm_set_enabled(LUA_PIO, (uint)px_sm, false);
    }
    if (tx_sm >= 0) {
        pio_sm_set_enabled(LUA_PIO, (uint)tx_sm, false);
    }
    if (rx_sm >= 0) {
        pio_sm_set_enabled(LUA_PIO, (uint)rx_sm, false);
    }

    /* Hand every Lua pad back to SIO and drive it low. gpio_init() clears the
     * function select, so a pad that a state machine was driving stops being
     * the PIO's regardless of what the program left behind -- the same move
     * arm_pump_stop() makes in boards/mk1c/pyro_board.c. */
    for (int i = 0; i < LUA_PIN_COUNT; i++) {
        gpio_init(lua_pins[i]);
        gpio_put(lua_pins[i], 0);
        gpio_set_dir(lua_pins[i], GPIO_OUT);
        gpio_put(lua_pins[i], 0);
    }

    /* So a later lua_plat_output_get() reports what the pin is actually
     * doing rather than what the dead script last asked for. */
    for (int i = 0; i < n_out; i++) {
        outs[i].value = 0;
    }
}
