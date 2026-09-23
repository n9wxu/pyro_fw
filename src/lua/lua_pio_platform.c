/*
 * Lua platform for RP2040 boards — shared implementation.
 *
 * Board-independent. Everything that differs between boards is the pin list,
 * which each board states in its own pin_caps.h alongside what every other
 * pin may become:
 *
 *      boards/mk1a/pin_caps.h   J6   GPIO18, GPIO19
 *      boards/mk1b/pin_caps.h   J1   GPIO8
 *      boards/mk1c/pin_caps.h   J3   GPIO18-21
 *
 * Three rules shape this file.
 *
 * 1. A Lua pin is only ever SIO or a PIO function, never a peripheral one.
 *    RP2040 fixes a pin's peripheral function by pin number, and on every one
 *    of these boards a Lua-reachable pin shares an I2C instance with the
 *    flight pressure sensor: GPIO18/19 are i2c1 on MK1C, whose MS5607 is on
 *    GPIO6/7. Do not call gpio_set_function with a peripheral argument
 *    anywhere below.
 *
 * 2. Core0 claims every PIO state machine, program offset and DMA channel
 *    here, at boot, before core1 exists. hw_claim_lock() takes spin lock 11,
 *    and a core1 killed inside it would strand that lock and hang core0's
 *    next claim.
 *
 * 3. Nothing here blocks: a full FIFO drops and a busy DMA skips a frame.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_platform.h"
#include "board_pins.h"
#include "pin_caps.h"
#include "pin_model.h"
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
 * Acquisition failure is not a runtime outcome to handle; it is a state the
 * build refuses to produce.
 *
 * pio1 has 32 instruction slots and 4 state machines, a pin holds one role,
 * and J3 has four pins, so the worst case an operator can configure is all
 * four in the three PIO-backed roles:
 *
 *      ws2812   4 instructions   1 SM
 *      uart_tx  4 instructions   1 SM
 *      uart_rx  8 instructions   1 SM
 *      ------------------------------
 *               16 instructions  3 SMs      (out of 32 and 4)
 *
 * Digital outputs and inputs are plain SIO and cost neither, so every
 * configuration fits with a state machine and half the instruction memory to
 * spare. The static_asserts below fail the build if a program outgrows that.
 *
 * Counted from the generated instruction arrays rather than the pio_program
 * structs: a struct member is not a constant expression, and a static_assert
 * that cannot see the number is not a check. */
#define LUA_PIO_PROG_LEN(p) (sizeof(p##_program_instructions) / sizeof(uint16_t))
#define LUA_PIO_BUDGET_INSTR                                                                                           \
    (LUA_PIO_PROG_LEN(lua_ws2812) + LUA_PIO_PROG_LEN(lua_uart_tx) + LUA_PIO_PROG_LEN(lua_uart_rx))
#define LUA_PIO_BUDGET_SMS 3

/* From boards/<name>/pin_caps.h. */
#define LUA_PIO LUA_PIO_INST
static const uint8_t lua_pins[LUA_PIN_COUNT] = LUA_PIN_LIST;

/* Lua addresses these by index through name lookup in pyro_lua.c, so a
 * resource configuration did not create cannot be named, let alone
 * reached. */

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
 * Do not use the hardware PWM slices: a slice keeps running after the
 * processor that set it stops, and an av-bay output must stop when the
 * program does. Same argument as the ARM_TOGGLE pump. */
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

    /* LUA_PIN_LIST and the capability table are two hand-written statements of
     * the same fact. A pin listed here but reserved in the table would hand a
     * script something the flight software owns, which no later check would
     * catch -- the roles below are indexed by position, not by capability. */
    int bad = pin_caps_check_lua_list();
    if (bad >= 0) {
        return bad; /* reported as a firmware bug by the caller */
    }

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

    /* Claim first, wire second. By the budget above none of these can fail,
     * so the non-asserting variants report rather than panic. */
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
    /* A script that outruns 9600 baud must not stall the core it runs on. */
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
    /* Do not call dma_channel_wait_for_finish_blocking() here. It looks
     * bounded at 30 us per pixel, but that holds only while the state machine
     * keeps draining the FIFO, and prove_core0.py refuses an unbounded wait
     * on core1. The next show() sends the current buffer anyway. */
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
/* One timer register read, no lock, safe from either core: the VM's
 * instruction hook calls this on core1. */
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
    /* Do not abort the pixel DMA. dma_channel_abort() spins until the abort
     * bit self-clears, and an in-flight transfer into a PIO TX FIFO cannot
     * complete while that FIFO is full and the state machine has stopped
     * draining it -- the state a wedged core1 leaves behind. That is an
     * unbounded wait on core0, on the path that exists so core0 never waits.
     *
     * Handing the pads back to SIO below is what makes the pin safe: once the
     * function select is SIO, neither PIO nor its DMA reaches a pin. A
     * pending transfer on a channel nothing will claim again costs nothing,
     * since core1 is never relaunched. */
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
