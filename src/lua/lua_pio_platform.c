/*
 * Lua platform for RP2040 boards -- shared implementation.
 *
 * Board-independent. What differs between boards is the pin list, which each
 * board states in its own pin_caps.h beside what every other pin may become.
 *
 * Three rules shape this file.
 *
 * 1. A Lua pin is only ever SIO or a PIO function, never a peripheral one.
 *    RP2040 fixes a pin's peripheral function by pin number, and on these
 *    boards a Lua-reachable pin shares an I2C instance with the flight
 *    pressure sensor: GPIO18/19 are i2c1 on MK1C, whose MS5607 is on
 *    GPIO6/7. Do not call gpio_set_function with a peripheral argument
 *    anywhere below.
 *
 * 2. Core0 claims every pad, PIO state machine, program offset and DMA
 *    channel here, at boot, before the scheduler starts, so the Lua task
 *    acquires nothing (lua_core1.h). A pad is claimed before it is touched.
 *
 * 3. Nothing here blocks: a full FIFO drops and a busy DMA skips a frame.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_platform.h"
#include "board_pins.h"
#include "pin_caps.h"
#include "pin_store.h"
#include "pin_model.h"
#include "lua_pio.pio.h"
#include "pyro_bridge.pio.h"
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
 * build refuses to produce. The worst case an operator can configure is every
 * PIO-backed Lua role at once -- ws2812, uart_tx, uart_rx -- and the
 * static_asserts in lua_plat_configure() hold that against pio1's 32
 * instruction slots and 4 state machines. Digital outputs and inputs are
 * SIO and cost neither.
 *
 * Counted from the generated instruction arrays rather than the pio_program
 * structs, whose members are not constant expressions. */
#define LUA_PIO_PROG_LEN(p) (sizeof(p##_program_instructions) / sizeof(uint16_t))
#define LUA_PIO_BUDGET_INSTR                                                                                           \
    (LUA_PIO_PROG_LEN(lua_ws2812) + LUA_PIO_PROG_LEN(lua_uart_tx) + LUA_PIO_PROG_LEN(lua_uart_rx))
#define LUA_PIO_BUDGET_SMS 3

/* ── The pyro PIO ─────────────────────────────────────────────────
 *
 * PIO0 belongs to the pyro hardware and PIO1 to Lua. A released pyro pad is
 * still a FET gate behind a fuse, so whatever drives it runs on the pyro
 * PIO, under pyro rules, whoever is commanding it; that also leaves Lua's
 * four state machines for Lua's own roles. The one program today is the
 * half-bridge, pyro_bridge_program.length instructions on one SM. MK1C's
 * ARM_TOGGLE pump also runs here and MK1C has no bridge, so the budget counts
 * the larger of the two rather than their sum. */
#define PYRO_PIO_BUDGET_INSTR LUA_PIO_PROG_LEN(pyro_bridge)
#define PYRO_PIO_BUDGET_SMS 1

#define LUA_PIO LUA_PIO_INST

#define LUA_MAX_OUT LUA_CFG_MAX
#define LUA_MAX_IN LUA_CFG_MAX
#define LUA_MAX_PIXELS 256

/* What a UART PIO program can be clocked for: 8 SM cycles a bit, and an
 * integer divider part of 1..65535 (RP2040 datasheet §3.5.5). */
#define UART_SM_CYCLES_PER_BIT 8u
#define PIO_CLKDIV_MAX 65535u

typedef struct {
    uint8_t pin;
    int value;
    bool dimmable;
} out_t;

typedef struct {
    uint8_t pin;
} in_t;

/* One more than the board's pads: a bridge is an output too, and it consumes
 * two released pyro pads rather than one entry in LUA_PIN_LIST. */
static out_t outs[LUA_MAX_OUT + 1];
static int n_out;
static in_t ins[LUA_MAX_IN];
static int n_in;

/* Every pad a successful publish handed to Lua, which is what safing puts
 * down. Never a pad whose publish was refused: that pad is someone else's. */
static uint8_t claimed[LUA_CFG_MAX + 2];
static int n_claimed;

static void remember_claim(uint32_t pads) {
    for (uint8_t pin = 0; pin < PAD_CLAIM_MAX_GPIO; pin++) {
        if ((pads & PAD(pin)) && n_claimed < (int)(sizeof(claimed) / sizeof(claimed[0]))) {
            claimed[n_claimed++] = pin;
        }
    }
}

/* Publish, and on success remember the pads it claimed. */
static bool publish(uint32_t pads, const char *name, lua_iface_kind_t kind, const void *vt, void *ctx) {
    if (lua_iface_publish(pads, name, kind, vt, ctx) < 0) {
        return false;
    }
    remember_claim(pads);
    return true;
}

static int tx_sm = -1, rx_sm = -1;

/* The half-bridge, on the pyro PIO (see above). */
static int bridge_sm = -1;
static uint32_t bridge_dropped;

static int px_count;
static int px_sm = -1;
static int px_dma = -1;
static uint32_t px_buf[LUA_MAX_PIXELS];  /* GRB<<8, the wire format */
static uint32_t px_wire[LUA_MAX_PIXELS]; /* what show() handed to DMA */

/* ── PWM by software, in the Lua task ─────────────────────────────
 *
 * Not the PWM slices: a slice drives two pins, and pins 16 apart share a
 * slice number, so two Lua pads could fight over one slice's period. On SIO
 * every pad has one owner, and safing it is the same gpio_init() as any
 * other pad. */
static uint32_t pwm_phase;

void lua_plat_pin_service(void) {
    pwm_phase = (pwm_phase + 1u) % LUA_PWM_STEPS;
    for (int i = 0; i < n_out; i++) {
        if (outs[i].dimmable) {
            gpio_put(outs[i].pin, lua_pwm_level(pwm_phase, outs[i].value));
        }
    }
}

/* ── The interfaces ───────────────────────────────────────────────
 *
 * ctx is the entry itself, so dispatch needs no index. Dimmability lives in
 * the vtable, so "PWM on a pin that cannot do it" is a table that was never
 * installed rather than a check that could be forgotten.
 *
 * Named *_vt because prove_core0.py folds exactly these symbols into the call
 * graph; renaming one drops whatever it points at out of the core1 proof
 * (DD-061). */

static void out_set(void *ctx, int value) {
    out_t *o = ctx;
    o->value = value;
    if (!o->dimmable) {
        gpio_put(o->pin, value > 0);
    }
}

static int out_get(void *ctx) {
    return ((out_t *)ctx)->value;
}

static void bridge_set(void *ctx, int value) {
    out_t *o = ctx;
    o->value = value;
    if (bridge_sm < 0) {
        return;
    }
    /* Never pio_sm_put_blocking(): a full FIFO means the script is pushing
     * levels faster than the dead band lets them out, so the level is dropped
     * and counted. */
    if (pio_sm_is_tx_fifo_full(PYRO_PIO_INST, (uint)bridge_sm)) {
        bridge_dropped++;
    } else {
        pio_sm_put(PYRO_PIO_INST, (uint)bridge_sm, (value > 0) ? 1u : 0u);
    }
}

static int in_get(void *ctx) {
    return gpio_get(((in_t *)ctx)->pin) ? 1 : 0;
}

static int serial_write(void *ctx, const char *s, int len) {
    (void)ctx;
    if (tx_sm < 0) {
        return 0;
    }
    int n = 0;
    while (n < len && !pio_sm_is_tx_fifo_full(LUA_PIO, (uint)tx_sm)) {
        pio_sm_put(LUA_PIO, (uint)tx_sm, (uint32_t)(uint8_t)s[n]);
        n++;
    }
    return n;
}

static int serial_read(void *ctx, char *buf, int max) {
    (void)ctx;
    if (rx_sm < 0) {
        return 0;
    }
    int n = 0;
    while (n < max && !pio_sm_is_rx_fifo_empty(LUA_PIO, (uint)rx_sm)) {
        /* Shifted right into a 32-bit ISR: the byte is the top 8 bits. */
        buf[n++] = (char)(pio_sm_get(LUA_PIO, (uint)rx_sm) >> 24);
    }
    return n;
}

static int px_len(void *ctx) {
    (void)ctx;
    return px_count;
}

static void px_set(void *ctx, int idx, uint8_t r, uint8_t g, uint8_t b) {
    (void)ctx;
    /* WS2812 wants GRB, MSB first; the SM autopulls 24 bits from the top. */
    px_buf[idx] = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
}

static void px_show(void *ctx) {
    (void)ctx;
    if (px_sm < 0 || px_count == 0) {
        return;
    }
    /* Not dma_channel_wait_for_finish_blocking(): it is bounded only while
     * the state machine drains the FIFO, and prove_core0.py refuses an
     * unbounded wait in the Lua task. The next show() sends the buffer. */
    if (dma_channel_is_busy((uint)px_dma)) {
        return;
    }
    memcpy(px_wire, px_buf, sizeof(uint32_t) * (size_t)px_count);
    dma_channel_set_read_addr((uint)px_dma, px_wire, false);
    dma_channel_set_trans_count((uint)px_dma, (uint32_t)px_count, true);
}

static const lua_if_output_t gpio_out_vt = {out_set, out_get, false};
static const lua_if_output_t gpio_pwm_vt = {out_set, out_get, true};
static const lua_if_output_t bridge_vt = {bridge_set, out_get, false};
static const lua_if_input_t gpio_in_vt = {in_get};
static const lua_if_serial_t pio_uart_vt = {serial_write, serial_read};
static const lua_if_pixel_t ws2812_vt = {px_len, px_set, px_show};

/* ── Boot-time configuration ──────────────────────────────────────── */

static uint px_offset, tx_offset, rx_offset;

static bool baud_usable(uint32_t baud) {
    if (baud == 0u) {
        return false;
    }
    uint32_t div = clock_get_hz(clk_sys) / (UART_SM_CYCLES_PER_BIT * baud);
    return div >= 1u && div <= PIO_CLKDIV_MAX;
}

/* A state machine and the program's slots, both or neither. */
static int claim_program(const pio_program_t *prog, uint *offset) {
    int sm = pio_claim_unused_sm(LUA_PIO, false);
    if (sm < 0) {
        return -1;
    }
    if (!pio_can_add_program(LUA_PIO, prog)) {
        pio_sm_unclaim(LUA_PIO, (uint)sm);
        return -1;
    }
    *offset = pio_add_program(LUA_PIO, prog);
    return sm;
}

static void configure_output(const lua_pin_cfg_t *c) {
    if (n_out >= LUA_MAX_OUT) {
        return;
    }
    out_t *o = &outs[n_out];
    o->pin = c->pin;
    o->value = 0;
    o->dimmable = (c->role == LUA_ROLE_PWM);
    if (!publish(PAD(c->pin), c->name, LUA_IF_OUTPUT, o->dimmable ? &gpio_pwm_vt : &gpio_out_vt, o)) {
        return;
    }
    n_out++;
    gpio_init(c->pin);
    gpio_set_dir(c->pin, GPIO_OUT);
    gpio_put(c->pin, 0);
}

static void configure_input(const lua_pin_cfg_t *c) {
    if (n_in >= LUA_MAX_IN) {
        return;
    }
    in_t *in = &ins[n_in];
    in->pin = c->pin;
    if (!publish(PAD(c->pin), c->name, LUA_IF_INPUT, &gpio_in_vt, in)) {
        return;
    }
    n_in++;
    gpio_init(c->pin);
    gpio_set_dir(c->pin, GPIO_IN);
    gpio_pull_down(c->pin);
}

static void configure_pixels(const lua_pin_cfg_t *c, int pixels) {
    if (px_sm < 0 || !publish(PAD(c->pin), c->name, LUA_IF_PIXEL, &ws2812_vt, NULL)) {
        return;
    }
    px_count = (pixels > LUA_MAX_PIXELS) ? LUA_MAX_PIXELS : pixels;
    lua_ws2812_program_init(LUA_PIO, (uint)px_sm, px_offset, c->pin);
    dma_channel_config dc = dma_channel_get_default_config((uint)px_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(LUA_PIO, (uint)px_sm, true));
    dma_channel_configure((uint)px_dma, &dc, &LUA_PIO->txf[px_sm], px_wire, (uint)px_count, false);
}

/* TX and RX are one resource to a script and can straddle two pads, so the
 * claim covers both at once: publishing on the first pad seen would leave
 * the second unclaimed. */
static void configure_serial(const lua_pin_cfg_t *tx, const lua_pin_cfg_t *rx, uint32_t baud) {
    uint32_t pads = (tx ? PAD(tx->pin) : PAD_NONE) | (rx ? PAD(rx->pin) : PAD_NONE);
    const char *name = tx ? tx->name : rx->name;
    if (!publish(pads, name, LUA_IF_SERIAL, &pio_uart_vt, NULL)) {
        return;
    }
    if (tx) {
        lua_uart_tx_program_init(LUA_PIO, (uint)tx_sm, tx_offset, tx->pin, baud);
    }
    if (rx) {
        lua_uart_rx_program_init(LUA_PIO, (uint)rx_sm, rx_offset, rx->pin, baud);
    }
}

int lua_plat_configure(const lua_pin_cfg_t *cfg, int n, uint32_t baud, int pixels) {
    static_assert(LUA_PIO_BUDGET_INSTR <= 32, "Lua PIO programs exceed pio1 instruction memory");
    static_assert(LUA_PIO_BUDGET_SMS <= 4, "Lua PIO roles exceed pio1 state machines");
    static_assert(PYRO_PIO_BUDGET_INSTR <= 32, "pyro PIO programs exceed pio0 instruction memory");
    static_assert(PYRO_PIO_BUDGET_SMS <= 4, "pyro PIO roles exceed pio0 state machines");
    static_assert(LUA_PIN_COUNT + 3 <= LUA_CFG_MAX, "this board's pads plus a released pyro block exceed LUA_CFG_MAX");

    /* LUA_PIN_LIST and the capability table state the same fact twice. A
     * listed pin the table reserves would hand a script something the flight
     * software owns. */
    if (pin_caps_check_lua_list() >= 0) {
        return LUA_PLAT_CLAIM_FAILED;
    }

    n_out = n_in = px_count = n_claimed = 0;
    lua_iface_reset();
    tx_sm = rx_sm = px_sm = px_dma = -1;

    const lua_pin_cfg_t *px = NULL, *tx = NULL, *rx = NULL;
    for (int i = 0; i < n && i < LUA_CFG_MAX; i++) {
        if (cfg[i].role == LUA_ROLE_PIXEL && !px) {
            px = &cfg[i];
        } else if (cfg[i].role == LUA_ROLE_TX && !tx) {
            tx = &cfg[i];
        } else if (cfg[i].role == LUA_ROLE_RX && !rx) {
            rx = &cfg[i];
        }
    }
    if ((tx || rx) && !baud_usable(baud)) {
        return LUA_PLAT_BAD_BAUD;
    }

    /* PIO and DMA first. By the budget above none of these can fail, so the
     * non-asserting variants report rather than panic. */
    if (px) {
        px_dma = dma_claim_unused_channel(false);
        px_sm = claim_program(&lua_ws2812_program, &px_offset);
        if (px_sm < 0 || px_dma < 0) {
            return LUA_PLAT_CLAIM_FAILED;
        }
    }
    if (tx && (tx_sm = claim_program(&lua_uart_tx_program, &tx_offset)) < 0) {
        return LUA_PLAT_CLAIM_FAILED;
    }
    if (rx && (rx_sm = claim_program(&lua_uart_rx_program, &rx_offset)) < 0) {
        return LUA_PLAT_CLAIM_FAILED;
    }

    for (int i = 0; i < n && i < LUA_CFG_MAX; i++) {
        switch (cfg[i].role) {
        case LUA_ROLE_OUT:
        case LUA_ROLE_PWM:
            configure_output(&cfg[i]);
            break;
        case LUA_ROLE_IN:
            configure_input(&cfg[i]);
            break;
        default:
            break;
        }
    }
    if (px) {
        configure_pixels(px, pixels);
    }
    if (tx || rx) {
        configure_serial(tx, rx, baud);
    }

    /* Last, so a board sees the generic roles already in place and adds to
     * them rather than racing them for a name. */
    board_lua_publish();
    return LUA_PLAT_OK;
}

/* ── Outputs ──────────────────────────────────────────────────────── */

uint32_t lua_plat_bridge_dropped(void) {
    return bridge_dropped;
}

int lua_plat_configure_bridge(uint8_t channel_pin, uint8_t common_pin, const char *name, unsigned deadtime_cycles) {
    if (n_out >= (int)(sizeof(outs) / sizeof(outs[0]))) {
        return -1;
    }
    int sm = pio_claim_unused_sm(PYRO_PIO_INST, false);
    if (sm < 0) {
        return -1;
    }
    if (!pio_can_add_program(PYRO_PIO_INST, &pyro_bridge_program)) {
        pio_sm_unclaim(PYRO_PIO_INST, (uint)sm);
        return -1;
    }

    /* Both pads or neither: a half-claimed bridge would be one FET gate this
     * side owns and one it does not. */
    out_t *o = &outs[n_out];
    memset(o, 0, sizeof(*o));
    o->pin = channel_pin;
    if (!publish(PAD(channel_pin) | PAD(common_pin), name, LUA_IF_OUTPUT, &bridge_vt, o)) {
        pio_sm_unclaim(PYRO_PIO_INST, (uint)sm);
        return -1;
    }
    n_out++;

    uint offset = pio_add_program(PYRO_PIO_INST, &pyro_bridge_program);
    pyro_bridge_program_init(PYRO_PIO_INST, (uint)sm, offset, channel_pin, common_pin, 1.0f);
    pio_sm_set_enabled(PYRO_PIO_INST, (uint)sm, true);
    bridge_sm = sm;

    /* The first word is the dead time, which the program keeps in Y; the
     * FIFO of a just-initialised state machine is empty. */
    pio_sm_put(PYRO_PIO_INST, (uint)sm, deadtime_cycles);
    return 0;
}

/* ── Flight state: read-only, invariant L11 (lua_platform.h) ─────── */

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
/* One timer register read, no lock, safe from either core. */
uint32_t lua_plat_now_us(void) {
    return time_us_32();
}

int lua_plat_pyro_adc(int channel) {
    return lua_flight_snapshot()->pyro_adc[(channel == 2) ? 1 : 0];
}

int lua_plat_pyro_released(int channel) {
    const pin_assign_t *a = pin_store_current();
    return (channel == 2) ? a->pyro2_released : a->pyro1_released;
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

/* ── Safing, from the flight task ─────────────────────────────────── */

void lua_plat_safe_outputs(void) {
    /* Not dma_channel_abort(): it spins until the abort bit clears, and a
     * transfer into a full PIO FIFO whose state machine has stopped never
     * completes -- an unbounded wait on the path that exists so core0 never
     * waits. Handing the pads to SIO below is what makes them safe: neither
     * PIO nor its DMA reaches a pin whose function is SIO. */
    if (px_sm >= 0) {
        pio_sm_set_enabled(LUA_PIO, (uint)px_sm, false);
    }
    if (tx_sm >= 0) {
        pio_sm_set_enabled(LUA_PIO, (uint)tx_sm, false);
    }
    if (rx_sm >= 0) {
        pio_sm_set_enabled(LUA_PIO, (uint)rx_sm, false);
    }
    if (bridge_sm >= 0) {
        pio_sm_set_enabled(PYRO_PIO_INST, (uint)bridge_sm, false);
    }

    /* gpio_init() selects SIO, whatever a state machine left behind. */
    for (int i = 0; i < n_claimed; i++) {
        gpio_init(claimed[i]);
        gpio_put(claimed[i], 0);
        gpio_set_dir(claimed[i], GPIO_OUT);
        gpio_put(claimed[i], 0);
    }

    /* So output.get() reports what the pin is doing, not what the stopped
     * script last asked for. */
    for (int i = 0; i < n_out; i++) {
        outs[i].value = 0;
    }
}
