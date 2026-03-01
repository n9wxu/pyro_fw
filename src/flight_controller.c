#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "pressure_sensor.h"
#include "tusb.h"
#include "bsp/board_api.h"


// GPIO Pins
#define PYRO_COMMON_EN 15
#define PYRO1_EN 21
#define PYRO2_EN 22
#define PYRO1_FAULT 17
#define PYRO2_FAULT 18
#define PYRO1_CONT_ADC 0
#define PYRO2_CONT_ADC 1
#define BUZZER 16

// Flight states
typedef enum { PAD_IDLE, LAUNCH, APOGEE, FALLING, LANDED } flight_state_t;

// Flight sample
typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa;
    int32_t altitude_cm;
    uint8_t state;
    uint8_t padding[3];
} flight_sample_t;

// Config
typedef struct {
    char id[9];
    char name[9];
    uint8_t pyro1_mode;
    uint16_t pyro1_distance;
    uint8_t pyro2_mode;
    uint16_t pyro2_distance;
} config_t;

// Forward declarations  
typedef struct flight_context_t flight_context_t;

// State function type - returns next state enum
typedef flight_state_t (*state_fn_t)(flight_context_t *, uint32_t);

// Flight context
typedef struct flight_context_t {
    flight_sample_t flight_buffer[4096];
    uint16_t buf_head;
    uint16_t buf_tail;
    uint16_t buf_count;
    uint16_t apogee_protect_idx;
    bool apogee_protected;
    int32_t ground_pressure;
    int32_t max_altitude;
    int32_t last_altitude;
    uint32_t launch_time;
    uint32_t last_sample;
    uint32_t last_telemetry;
    bool pyro1_fired;
    bool pyro2_fired;
    uint8_t pyro_firing;
    uint32_t pyro_fire_start;
    config_t config;
    flight_state_t current_state;
    int32_t filtered_pressure;
    bool filter_initialized;
} flight_context_t;

// State function array
static state_fn_t state_functions[5];

// Continuity check
uint16_t adc_oversample(uint8_t channel, uint16_t samples) {
    uint32_t sum = 0;
    for (uint16_t i = 0; i < samples; i++) {
        adc_select_input(channel);
        sum += adc_read();
    }
    // 256 samples gives 4 extra bits (12-bit -> 16-bit)
    // Sum of 256 12-bit values, shift right by 4 to get 16-bit result
    return (sum >> 4);
}

void check_continuity(uint16_t *pyro1, uint16_t *pyro2) {
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);
    gpio_put(PYRO_COMMON_EN, 1);
    sleep_ms(10);
    *pyro1 = adc_oversample(PYRO1_CONT_ADC, 256);
    *pyro2 = adc_oversample(PYRO2_CONT_ADC, 256);
    gpio_put(PYRO_COMMON_EN, 0);
}

// Buffer operations
void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    if (ctx->buf_count == 4096) {
        if (ctx->apogee_protected && ctx->buf_tail == ctx->apogee_protect_idx) return;
        ctx->buf_tail = (ctx->buf_tail + 1) % 4096;
        ctx->buf_count--;
    }
    ctx->flight_buffer[ctx->buf_head].time_ms = time_ms;
    ctx->flight_buffer[ctx->buf_head].pressure_pa = pressure;
    ctx->flight_buffer[ctx->buf_head].altitude_cm = altitude;
    ctx->flight_buffer[ctx->buf_head].state = st;
    ctx->buf_head = (ctx->buf_head + 1) % 4096;
    ctx->buf_count++;
}

// Filter pressure with 1-second time constant
// alpha = dt / (tau + dt), tau = 1.0s
int32_t filter_pressure(flight_context_t *ctx, int32_t raw_pressure, uint32_t dt_ms) {
    if (!ctx->filter_initialized) {
        ctx->filtered_pressure = raw_pressure;
        ctx->filter_initialized = true;
        return raw_pressure;
    }
    // alpha = dt / (1000 + dt), scaled by 1000 for integer math
    // filtered = filtered + alpha * (raw - filtered)
    int32_t alpha = (dt_ms * 1000) / (1000 + dt_ms);
    ctx->filtered_pressure += ((raw_pressure - ctx->filtered_pressure) * alpha) / 1000;
    return ctx->filtered_pressure;
}

// Convert pressure to altitude (barometric formula, integer math)
// h = (1 - (P/P0)^0.190284) * 44330.8 cm
int32_t pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    // Simplified: h ≈ (P0 - P) * 8.3 cm/Pa (valid for small altitude changes)
    return ((ground_pressure_pa - pressure_pa) * 83) / 10;
}

// Telemetry
void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state) {
    char buf[128];
    int len = 0;
    
    len += snprintf(buf + len, sizeof(buf) - len, "{%03ld>", altitude_cm / 3048);
    len += snprintf(buf + len, sizeof(buf) - len, "@%d>", state == PAD_IDLE ? 1 : state == LAUNCH ? 2 : state == APOGEE ? 5 : state == FALLING ? 8 : 9);
    len += snprintf(buf + len, sizeof(buf) - len, "#%04lu>", time_ms / 1000);
    
    if (state == PAD_IDLE) {
        len += snprintf(buf + len, sizeof(buf) - len, "~AB---->");
    } else {
        len += snprintf(buf + len, sizeof(buf) - len, "~%c%c---->", ctx->pyro1_fired ? '1' : 'A', ctx->pyro2_fired ? '2' : 'B');
    }
    
    if (state >= APOGEE) {
        len += snprintf(buf + len, sizeof(buf) - len, "%%0%04ld>", ctx->max_altitude / 30);
    }
    
    len += snprintf(buf + len, sizeof(buf) - len, "=%s>", ctx->config.name);
    
    uart_puts(uart0, buf);
}

// Fire pyro (non-blocking start)
void fire_pyro(flight_context_t *ctx, uint8_t channel) {
    gpio_put(PYRO_COMMON_EN, 1);
    gpio_put(channel == 1 ? PYRO1_EN : PYRO2_EN, 1);
    ctx->pyro_firing = channel;
    ctx->pyro_fire_start = to_ms_since_boot(get_absolute_time());
}

// Update pyro fire state (call in main loop)
void update_pyro_fire(flight_context_t *ctx, uint32_t now) {
    if (ctx->pyro_firing && (now - ctx->pyro_fire_start >= 500)) {
        gpio_put(ctx->pyro_firing == 1 ? PYRO1_EN : PYRO2_EN, 0);
        gpio_put(PYRO_COMMON_EN, 0);
        if (ctx->pyro_firing == 1) ctx->pyro1_fired = true;
        else ctx->pyro2_fired = true;
        ctx->pyro_firing = 0;
    }
}

// Forward declarations
flight_state_t state_pad_idle(flight_context_t *ctx, uint32_t now);
flight_state_t state_launch(flight_context_t *ctx, uint32_t now);
flight_state_t state_apogee(flight_context_t *ctx, uint32_t now);
flight_state_t state_falling(flight_context_t *ctx, uint32_t now);
flight_state_t state_landed(flight_context_t *ctx, uint32_t now);

// State: PAD_IDLE
flight_state_t state_pad_idle(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 10) return PAD_IDLE;
    
    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = (ctx->last_sample > 0) ? (now - ctx->last_sample) : 10;
    int32_t filtered = filter_pressure(ctx, (int32_t)(int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    uint32_t flight_time = 0;
    buf_add(ctx, flight_time, filtered, altitude, PAD_IDLE);
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    
    if (altitude > 1000) {
        ctx->launch_time = now;
        return LAUNCH;
    }
    return PAD_IDLE;
}

// State: LAUNCH
flight_state_t state_launch(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 100) return LAUNCH;
    
    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, LAUNCH);
    
    if (altitude > ctx->max_altitude) {
        ctx->max_altitude = altitude;
    } else if (altitude < ctx->max_altitude - 1000) {
        ctx->apogee_protect_idx = (ctx->buf_head >= 50) ? (ctx->buf_head - 50) : (4096 + ctx->buf_head - 50);
        ctx->apogee_protected = true;
        ctx->last_altitude = altitude;
        ctx->last_sample = now;
        return APOGEE;
    }
    
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return LAUNCH;
}

// State: APOGEE
flight_state_t state_apogee(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 100) return APOGEE;
    
    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, APOGEE);
    
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return FALLING;
}

// State: FALLING
flight_state_t state_falling(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 50) return FALLING;
    
    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, FALLING);
    
    int32_t fallen = (ctx->max_altitude - altitude) / 100;
    int32_t agl = altitude / 100;
    
    if (!ctx->pyro1_fired && !ctx->pyro_firing && 
        ((ctx->config.pyro1_mode == 1 && fallen >= ctx->config.pyro1_distance) || 
         (ctx->config.pyro1_mode == 2 && agl <= ctx->config.pyro1_distance))) {
        fire_pyro(ctx, 1);
    }
    if (!ctx->pyro2_fired && !ctx->pyro_firing && 
        ((ctx->config.pyro2_mode == 1 && fallen >= ctx->config.pyro2_distance) || 
         (ctx->config.pyro2_mode == 2 && agl <= ctx->config.pyro2_distance))) {
        fire_pyro(ctx, 2);
    }
    
    if (altitude < 500 && abs(altitude - ctx->last_altitude) < 100) {
        ctx->last_altitude = altitude;
        ctx->last_sample = now;
        return LANDED;
    }
    
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return FALLING;
}

// State: LANDED
flight_state_t state_landed(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 1000) return LANDED;
    
    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, LANDED);
    
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return LANDED;
}

int main() {
    // Initialize board first (sets up clocks, etc)
    board_init();
    
    // Initialize TinyUSB BEFORE stdio
    tud_init(BOARD_TUD_RHPORT);
    
    // Initialize stdio (USB and/or UART)
    stdio_init_all();
    
    printf("Pyro MK1B Flight Computer\n");
    
    // Init UART0 for telemetry
    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    
    // Init I2C
    i2c_init(i2c0, 400000);
    gpio_set_function(8, GPIO_FUNC_I2C);
    gpio_set_function(9, GPIO_FUNC_I2C);
    gpio_pull_up(8);
    gpio_pull_up(9);
    
    // Init ADC
    adc_init();
    adc_gpio_init(26);
    adc_gpio_init(27);
    
    // Init pyro GPIOs
    gpio_init(PYRO_COMMON_EN);
    gpio_init(PYRO1_EN);
    gpio_init(PYRO2_EN);
    gpio_init(BUZZER);
    gpio_set_dir(PYRO_COMMON_EN, GPIO_OUT);
    gpio_set_dir(PYRO1_EN, GPIO_OUT);
    gpio_set_dir(PYRO2_EN, GPIO_OUT);
    gpio_set_dir(BUZZER, GPIO_OUT);
    gpio_put(PYRO_COMMON_EN, 0);
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);
    gpio_put(BUZZER, 0);
    
    // Init pressure sensor
    if (pressure_sensor_init() != 0) {
        while (1) sleep_ms(1000);
    }
    
    // Init context
    flight_context_t ctx = {0};
    ctx.config = (config_t){"PYRO001", "PYRO001", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    
    // Initialize state function array
    state_functions[PAD_IDLE] = state_pad_idle;
    state_functions[LAUNCH] = state_launch;
    state_functions[APOGEE] = state_apogee;
    state_functions[FALLING] = state_falling;
    state_functions[LANDED] = state_landed;
    
    // Continuity check
    uint16_t cont1, cont2;
    check_continuity(&cont1, &cont2);
    
    // Calibrate ground pressure
    int32_t sum = 0;
    for (int i = 0; i < 10; i++) {
        pressure_reading_t data;
        pressure_sensor_read(&data);
        sum += data.pressure_pa;
        sleep_ms(100);
    }
    ctx.ground_pressure = sum / 10;
    
    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        tud_task();

        // Update pyro fire state
        update_pyro_fire(&ctx, now);
        
        // Run current state and get next state
        ctx.current_state = state_functions[ctx.current_state](&ctx, now);
        
        // Telemetry at 1Hz
        if (now - ctx.last_telemetry >= 1000) {
            pressure_reading_t pdata;
            pressure_sensor_read(&pdata);
            int32_t altitude = pressure_to_altitude_cm((int32_t)pdata.pressure_pa, ctx.ground_pressure);
            uint32_t flight_time = (ctx.current_state != PAD_IDLE) ? (now - ctx.launch_time) : 0;
            
            send_telemetry(&ctx, flight_time, altitude, ctx.current_state);
            ctx.last_telemetry = now;
        }
        
        sleep_ms(10);
    }
    
    return 0;
}
