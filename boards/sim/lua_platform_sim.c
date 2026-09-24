/*
 * Lua platform — simulator.
 *
 * Implements src/lua/lua_platform.h against simulated hardware, so the API,
 * the sandbox and user scripts can be developed and tested with no board
 * attached. Pin states and the UART buffers are exported for the WASM host to
 * render, which is what makes a script's effect visible.
 *
 * Deliberately mirrors the MK1C resource set — three outputs, one input, one
 * serial — so a script written here runs unchanged on hardware.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_platform.h"
#include <stdint.h>
#include <time.h>
#include "lua_platform_cfg.h"
#include <string.h>

/* ── Simulated resources ──────────────────────────────────────────── */

/* The default set, which is also what the WASM UI renders. lua_plat_configure()
 * can replace it, so the simulator honours the same contract the board does
 * and a script written here meets the same resource rules. */
/* The index into these is the ctx a vtable receives. */
static int output_val[4];
static int input_val[4];

#define SIM_PIXELS 16
static int px_configured = SIM_PIXELS;

static char cfg_names[4][LUA_NAME_MAX];

/* ── The interfaces ───────────────────────────────────────────────
 *
 * ctx is the slot index, passed as a value rather than a pointer, because
 * here a resource is nothing but its index. Named *_vt: prove_core0.py folds
 * exactly that suffix into the call graph, and the simulator holding to the
 * convention is what keeps the two implementations checkable the same way. */

#define SLOT(ctx) ((int)(intptr_t)(ctx))
#define CTX(i) ((void *)(intptr_t)(i))

static void sim_out_set(void *ctx, int value) {
    output_val[SLOT(ctx)] = value;
}
static int sim_out_get(void *ctx) {
    return output_val[SLOT(ctx)];
}
static int sim_in_get(void *ctx) {
    return input_val[SLOT(ctx)];
}
static int sim_serial_write(void *ctx, const char *s, int len);
static int sim_serial_read(void *ctx, char *buf, int max);
static int sim_px_count(void *ctx);
static void sim_px_set(void *ctx, int idx, uint8_t r, uint8_t g, uint8_t b);
static void sim_px_show(void *ctx);

static const lua_if_output_t sim_out_vt = {sim_out_set, sim_out_get, false};
static const lua_if_output_t sim_pwm_vt = {sim_out_set, sim_out_get, true};
static const lua_if_input_t sim_in_vt = {sim_in_get};
static const lua_if_serial_t sim_serial_vt = {sim_serial_write, sim_serial_read};
static const lua_if_pixel_t sim_pixel_vt = {sim_px_count, sim_px_set, sim_px_show};

/* A configured entry carries its own pin; the demo set has none, so the
 * simulator numbers its notional pads from 18 the way MK1C's J3 does. */
static uint32_t sim_pad(const lua_pin_cfg_t *cfg, int i) {
    uint8_t pin = cfg[i].pin ? cfg[i].pin : (uint8_t)(18 + i);
    return (pin < PAD_CLAIM_MAX_GPIO) ? PAD(pin) : PAD_NONE;
}

/* The default set, which is also what the WASM UI renders.
 * lua_plat_configure() replaces it, so the simulator honours the same
 * contract the board does and a script written here meets the same resource
 * rules.
 *
 * A constructor because on a board the resources exist before anything asks:
 * lua_plat_configure() runs at boot, long before pyro_lua_init(). The WASM
 * host and the host tests start the VM without configuring anything, and a
 * simulated board that came up with an empty table would make them exercise a
 * state the target never has. */
__attribute__((constructor)) static void publish_demo_set(void) {
    lua_iface_reset();
    /* Notional pads, so the simulator spends claims the way the board does
     * and a resource that could not be claimed does not appear here either. */
    lua_iface_publish(PAD(18), "beacon", LUA_IF_OUTPUT, &sim_pwm_vt, CTX(0)); /* night-launch LED */
    lua_iface_publish(PAD(19), "strobe", LUA_IF_OUTPUT, &sim_pwm_vt, CTX(1));
    lua_iface_publish(PAD(20), "aux", LUA_IF_OUTPUT, &sim_out_vt, CTX(2)); /* digital only */
    lua_iface_publish(PAD(21), "sense", LUA_IF_INPUT, &sim_in_vt, CTX(0));
    lua_iface_publish(PAD(22), "radio", LUA_IF_SERIAL, &sim_serial_vt, NULL);
    lua_iface_publish(PAD(23), "string", LUA_IF_PIXEL, &sim_pixel_vt, NULL);
}

int lua_plat_pin_count(void) {
    return 4;
}

void lua_plat_pin_service(void) {
    /* Software PWM is a hardware concern; the simulator shows the duty value
     * directly, which is what the UI renders as brightness. */
}

int lua_plat_configure(const lua_pin_cfg_t *cfg, int n, unsigned baud, int pixels) {
    (void)baud;
    if (!cfg || n <= 0) {
        publish_demo_set();
        return 0;
    }

    lua_iface_reset();
    int n_out = 0, n_in = 0;
    bool serial_published = false;
    for (int i = 0; i < n && i < 4; i++) {
        strncpy(cfg_names[i], cfg[i].name ? cfg[i].name : "", LUA_NAME_MAX - 1);
        cfg_names[i][LUA_NAME_MAX - 1] = '\0';
        switch (cfg[i].role) {
        case LUA_ROLE_OUT:
        case LUA_ROLE_PWM:
            output_val[n_out] = 0;
            lua_iface_publish(sim_pad(cfg, i), cfg_names[i], LUA_IF_OUTPUT,
                              cfg[i].role == LUA_ROLE_PWM ? &sim_pwm_vt : &sim_out_vt, CTX(n_out));
            n_out++;
            break;
        case LUA_ROLE_IN:
            lua_iface_publish(sim_pad(cfg, i), cfg_names[i], LUA_IF_INPUT, &sim_in_vt, CTX(n_in));
            n_in++;
            break;
        case LUA_ROLE_TX:
        case LUA_ROLE_RX:
            if (!serial_published) {
                lua_iface_publish(sim_pad(cfg, i), cfg_names[i], LUA_IF_SERIAL, &sim_serial_vt, NULL);
                serial_published = true;
            }
            break;
        default:
            break;
        }
    }
    px_configured = (pixels > SIM_PIXELS) ? SIM_PIXELS : pixels;
    if (px_configured > 0) {
        lua_iface_publish(PAD(23), "string", LUA_IF_PIXEL, &sim_pixel_vt, NULL);
    }
    board_lua_publish();
    return 0;
}

/* ── Simulated UART ───────────────────────────────────────────────── */

#define SIM_UART_BUF 2048

static char tx_buf[SIM_UART_BUF]; /* script -> outside world */
static int tx_len;
static char rx_buf[SIM_UART_BUF]; /* outside world -> script */
static int rx_head, rx_tail;

#define SIM_CONSOLE_BUF 4096
static char console_buf[SIM_CONSOLE_BUF];
static int console_len;

/* ── Simulated LED string ─────────────────────────────────────────
 *
 * On hardware this buffer is DMA'd to a PIO state machine clocking WS2812
 * timing. Here it is just memory, with a counter so the UI can show that
 * show() -- and only show() -- reaches the wire. */

static uint8_t pixel_buf[SIM_PIXELS * 3]; /* R,G,B per LED */
static uint8_t pixel_wire[SIM_PIXELS * 3];
static uint32_t pixel_shows;

static int sim_px_count(void *ctx) {
    (void)ctx;
    return px_configured;
}

static void sim_px_set(void *ctx, int idx, uint8_t r, uint8_t g, uint8_t b) {
    (void)ctx;
    pixel_buf[idx * 3 + 0] = r;
    pixel_buf[idx * 3 + 1] = g;
    pixel_buf[idx * 3 + 2] = b;
}

static void sim_px_show(void *ctx) {
    (void)ctx;
    memcpy(pixel_wire, pixel_buf, sizeof(pixel_wire));
    pixel_shows++;
}

/* ── Flight state, injected by the simulation ─────────────────────── */

static int32_t sim_pressure_pa = 101325;
static int32_t sim_altitude_cm;
static int32_t sim_speed_cms;
static int32_t sim_max_alt_cm;
static int sim_flight_state;
static uint32_t sim_time_ms;
static int sim_pyro_status[2];
static int sim_pyro_adc[2];
static int sim_under_thrust;
static int sim_apogee_detected;
static uint32_t sim_telem_seq;

#define SIM_LOG_BUF 4096
static char sim_log_buf[SIM_LOG_BUF];
static int sim_log_len;
static uint32_t sim_log_dropped;

/* ── lua_platform.h implementation ────────────────────────────────── */

static int sim_serial_write(void *ctx, const char *s, int len) {
    (void)ctx;
    int room = SIM_UART_BUF - tx_len;
    if (len > room)
        len = room;
    if (len > 0) {
        memcpy(tx_buf + tx_len, s, (size_t)len);
        tx_len += len;
    }
    return len;
}

static int sim_serial_read(void *ctx, char *buf, int max) {
    (void)ctx;
    int n = 0;
    while (n < max && rx_tail != rx_head) {
        buf[n++] = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % SIM_UART_BUF;
    }
    return n;
}

int32_t lua_plat_pressure_pa(void) {
    return sim_pressure_pa;
}
int32_t lua_plat_altitude_cm(void) {
    return sim_altitude_cm;
}
int32_t lua_plat_speed_cms(void) {
    return sim_speed_cms;
}
int32_t lua_plat_max_altitude_cm(void) {
    return sim_max_alt_cm;
}
int lua_plat_flight_state(void) {
    return sim_flight_state;
}
uint32_t lua_plat_time_ms(void) {
    return sim_time_ms;
}
int lua_plat_pyro_status(int channel) {
    return sim_pyro_status[(channel == 2) ? 1 : 0];
}
/* The simulator has no timer peripheral, so a monotonic host clock. Must be
 * real time rather than a counter: the VM host time-boxes work units with it,
 * and a fake clock would make every slice either instant or infinite. */
uint32_t lua_plat_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)(t.tv_nsec / 1000));
}

int lua_plat_pyro_adc(int channel) {
    return sim_pyro_adc[(channel == 2) ? 1 : 0];
}

/* The simulator has no pin assignment, so nothing is ever released here. */
int lua_plat_pyro_released(int channel) {
    (void)channel;
    return 0;
}
int lua_plat_under_thrust(void) {
    return sim_under_thrust;
}
int lua_plat_apogee_detected(void) {
    return sim_apogee_detected;
}
uint32_t lua_plat_telem_seq(void) {
    return sim_telem_seq;
}

/* On the target this hands bytes to core0, which owns the log file. Here
 * there is one core and no flash, but the buffer keeps the simulator a
 * faithful bench: a script that floods the log drops output in both places
 * rather than only on hardware. */
void lua_plat_log_write(const char *s, int len) {
    int room = SIM_LOG_BUF - 1 - sim_log_len;
    if (len > room) {
        len = room;
        sim_log_dropped++;
    }
    if (len > 0) {
        memcpy(sim_log_buf + sim_log_len, s, (size_t)len);
        sim_log_len += len;
        sim_log_buf[sim_log_len] = '\0';
    }
}

void lua_plat_console_out(const char *s, int len) {
    int room = SIM_CONSOLE_BUF - 1 - console_len;
    if (len > room) {
        /* Drop the oldest half rather than the newest output: when a script
         * is spewing, the recent lines are the ones being debugged. */
        int keep = console_len / 2;
        memmove(console_buf, console_buf + console_len - keep, (size_t)keep);
        console_len = keep;
        room = SIM_CONSOLE_BUF - 1 - console_len;
        if (len > room)
            len = room;
    }
    if (len > 0) {
        memcpy(console_buf + console_len, s, (size_t)len);
        console_len += len;
        console_buf[console_len] = '\0';
    }
}

/* ── Simulation-side hooks (exported to the WASM host) ────────────── */

void sim_lua_set_flight(int state, int32_t alt_cm, int32_t speed_cms, int32_t pressure_pa, int32_t max_alt_cm,
                        uint32_t time_ms) {
    sim_flight_state = state;
    sim_altitude_cm = alt_cm;
    sim_speed_cms = speed_cms;
    sim_pressure_pa = pressure_pa;
    sim_max_alt_cm = max_alt_cm;
    sim_time_ms = time_ms;
}

void sim_lua_set_pyro(int channel, int status) {
    sim_pyro_status[(channel == 2) ? 1 : 0] = status;
}

void sim_lua_set_pyro_adc(int channel, int counts) {
    sim_pyro_adc[(channel == 2) ? 1 : 0] = counts;
}

void sim_lua_set_thrust(int under_thrust, int apogee_detected) {
    sim_under_thrust = under_thrust;
    sim_apogee_detected = apogee_detected;
}

const char *sim_lua_log(void) {
    return sim_log_buf;
}

void sim_lua_log_clear(void) {
    sim_log_len = 0;
    sim_log_buf[0] = '\0';
}

uint32_t sim_lua_log_dropped(void) {
    return sim_log_dropped;
}

/* The UI panes read the resource table, the same way a script does, so what
 * they render cannot drift from what the script can reach. */
static const lua_resource_t *nth(lua_iface_kind_t kind, int idx) {
    return idx < 0 ? NULL : lua_iface_nth_of_kind(kind, idx);
}

void sim_lua_set_input(int idx, int value) {
    const lua_resource_t *r = nth(LUA_IF_INPUT, idx);
    if (r)
        input_val[SLOT(r->ctx)] = value ? 1 : 0;
}

int sim_lua_output_count(void) {
    return lua_iface_count_kind(LUA_IF_OUTPUT);
}
const char *sim_lua_output_name(int idx) {
    const lua_resource_t *r = nth(LUA_IF_OUTPUT, idx);
    return r ? r->name : "";
}
int sim_lua_output_value(int idx) {
    const lua_resource_t *r = nth(LUA_IF_OUTPUT, idx);
    return r ? ((const lua_if_output_t *)r->vt)->get(r->ctx) : 0;
}
int sim_lua_output_dimmable(int idx) {
    const lua_resource_t *r = nth(LUA_IF_OUTPUT, idx);
    return r ? ((const lua_if_output_t *)r->vt)->dimmable : 0;
}

int sim_lua_input_count(void) {
    return lua_iface_count_kind(LUA_IF_INPUT);
}
const char *sim_lua_input_name(int idx) {
    const lua_resource_t *r = nth(LUA_IF_INPUT, idx);
    return r ? r->name : "";
}

int sim_lua_serial_count(void) {
    return lua_iface_count_kind(LUA_IF_SERIAL);
}
const char *sim_lua_serial_name(int idx) {
    const lua_resource_t *r = nth(LUA_IF_SERIAL, idx);
    return r ? r->name : "";
}

/* Drain what the script transmitted, for the terminal pane. */
const char *sim_lua_uart_tx(void) {
    tx_buf[tx_len < SIM_UART_BUF ? tx_len : SIM_UART_BUF - 1] = '\0';
    return tx_buf;
}
void sim_lua_uart_tx_clear(void) {
    tx_len = 0;
}

/* Feed the script's serial.read() from the terminal pane. */
void sim_lua_uart_rx_push(const char *s) {
    while (*s) {
        int next = (rx_head + 1) % SIM_UART_BUF;
        if (next == rx_tail)
            break; /* full: drop, as a real UART would */
        rx_buf[rx_head] = *s++;
        rx_head = next;
    }
}

/* What is actually on the wire -- the last show(), not the working buffer,
 * so the UI shows a script that forgets show() as a string that never lights. */
int sim_lua_pixel_count(void) {
    return SIM_PIXELS;
}
int sim_lua_pixel_r(int i) {
    return (i >= 0 && i < SIM_PIXELS) ? pixel_wire[i * 3 + 0] : 0;
}
int sim_lua_pixel_g(int i) {
    return (i >= 0 && i < SIM_PIXELS) ? pixel_wire[i * 3 + 1] : 0;
}
int sim_lua_pixel_b(int i) {
    return (i >= 0 && i < SIM_PIXELS) ? pixel_wire[i * 3 + 2] : 0;
}
uint32_t sim_lua_pixel_shows(void) {
    return pixel_shows;
}

const char *sim_lua_console(void) {
    return console_buf;
}
void sim_lua_console_clear(void) {
    console_len = 0;
    console_buf[0] = '\0';
}

/* Core0 kills core1 and then puts its outputs down. On the simulator there is
 * no second core and no hardware, but the visible state has to match the
 * target's or the sim stops being a faithful bench for the failure. */
void lua_plat_safe_outputs(void) {
    for (unsigned i = 0; i < sizeof(output_val) / sizeof(output_val[0]); i++) {
        output_val[i] = 0;
    }
}
