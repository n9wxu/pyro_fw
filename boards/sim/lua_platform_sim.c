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
#include "lua_platform_cfg.h"
#include <string.h>

/* ── Simulated resources ──────────────────────────────────────────── */

/* The default set, which is also what the WASM UI renders. lua_plat_configure()
 * can replace it, so the simulator honours the same contract the board does
 * and a script written here meets the same resource rules. */
static lua_output_desc_t outputs[] = {
    {"beacon", true}, /* J3 pad, dimmable: night-launch LED */
    {"strobe", true}, /* J3 pad, dimmable                   */
    {"aux", false},   /* J3 pad, digital only               */
};
static int n_outputs = 3;
static int output_val[4];

static lua_input_desc_t inputs[] = {
    {"sense"}, /* J3 pad configured as an input */
};
static int n_inputs = 1;
static int input_val[4];

static lua_serial_desc_t serials[] = {
    {"radio"}, /* PIO UART on a J3 pad */
};
static int n_serials = 1;

#define SIM_PIXELS 16
static int px_configured = SIM_PIXELS;

static char cfg_names[4][LUA_NAME_MAX];

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
        return 0; /* keep the built-in demo set */
    }
    n_outputs = n_inputs = n_serials = 0;
    for (int i = 0; i < n && i < 4; i++) {
        strncpy(cfg_names[i], cfg[i].name ? cfg[i].name : "", LUA_NAME_MAX - 1);
        cfg_names[i][LUA_NAME_MAX - 1] = '\0';
        switch (cfg[i].role) {
        case LUA_ROLE_OUT:
        case LUA_ROLE_PWM:
            outputs[n_outputs].name = cfg_names[i];
            outputs[n_outputs].dimmable = (cfg[i].role == LUA_ROLE_PWM);
            output_val[n_outputs] = 0;
            n_outputs++;
            break;
        case LUA_ROLE_IN:
            inputs[n_inputs].name = cfg_names[i];
            n_inputs++;
            break;
        case LUA_ROLE_TX:
        case LUA_ROLE_RX:
            if (n_serials == 0) {
                serials[0].name = cfg_names[i];
                n_serials = 1;
            }
            break;
        default:
            break;
        }
    }
    px_configured = (pixels > SIM_PIXELS) ? SIM_PIXELS : pixels;
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

int lua_plat_pixel_count(void) {
    return px_configured;
}

void lua_plat_pixel_set(int idx, uint8_t r, uint8_t g, uint8_t b) {
    pixel_buf[idx * 3 + 0] = r;
    pixel_buf[idx * 3 + 1] = g;
    pixel_buf[idx * 3 + 2] = b;
}

void lua_plat_pixel_show(void) {
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

/* ── lua_platform.h implementation ────────────────────────────────── */

int lua_plat_output_count(void) {
    return n_outputs;
}
const lua_output_desc_t *lua_plat_output_desc(int idx) {
    return &outputs[idx];
}
void lua_plat_output_set(int idx, int value) {
    output_val[idx] = value;
}
int lua_plat_output_get(int idx) {
    return output_val[idx];
}

int lua_plat_input_count(void) {
    return n_inputs;
}
const lua_input_desc_t *lua_plat_input_desc(int idx) {
    return &inputs[idx];
}
int lua_plat_input_get(int idx) {
    return input_val[idx];
}

int lua_plat_serial_count(void) {
    return n_serials;
}
const lua_serial_desc_t *lua_plat_serial_desc(int idx) {
    return &serials[idx];
}

int lua_plat_serial_write(int idx, const char *s, int len) {
    (void)idx;
    int room = SIM_UART_BUF - tx_len;
    if (len > room)
        len = room;
    if (len > 0) {
        memcpy(tx_buf + tx_len, s, (size_t)len);
        tx_len += len;
    }
    return len;
}

int lua_plat_serial_read(int idx, char *buf, int max) {
    (void)idx;
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

void sim_lua_set_input(int idx, int value) {
    if (idx >= 0 && idx < lua_plat_input_count())
        input_val[idx] = value ? 1 : 0;
}

int sim_lua_output_count(void) {
    return lua_plat_output_count();
}
const char *sim_lua_output_name(int idx) {
    return (idx >= 0 && idx < lua_plat_output_count()) ? outputs[idx].name : "";
}
int sim_lua_output_value(int idx) {
    return (idx >= 0 && idx < lua_plat_output_count()) ? output_val[idx] : 0;
}
int sim_lua_output_dimmable(int idx) {
    return (idx >= 0 && idx < lua_plat_output_count()) ? outputs[idx].dimmable : 0;
}

int sim_lua_input_count(void) {
    return lua_plat_input_count();
}
const char *sim_lua_input_name(int idx) {
    return (idx >= 0 && idx < lua_plat_input_count()) ? inputs[idx].name : "";
}

int sim_lua_serial_count(void) {
    return lua_plat_serial_count();
}
const char *sim_lua_serial_name(int idx) {
    return (idx >= 0 && idx < lua_plat_serial_count()) ? serials[idx].name : "";
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
    for (int i = 0; i < n_outputs; i++) {
        output_val[i] = 0;
    }
}
