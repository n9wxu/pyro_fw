#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "pressure_sensor.h"
#include "pyro.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include <lfs.h>
#include "device_status.h"
#include <pico_fota_bootloader/core.h>
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "version.h"

volatile device_status_t g_status = {0};

/* Network interface */
void net_mac_init(void);
void net_init(void);
void net_start(void);
void net_mdns_poll(void);
void net_service(void);
void http_server_init(void);


// GPIO Pins
#define BUZZER 16

// System states (boot + flight)
typedef enum {
    // Boot sequence
    BOOT_FILESYSTEM,
    BOOT_I2C_SETTLE,
    BOOT_SENSOR_DETECT,
    BOOT_PYRO_INIT,
    BOOT_CONTINUITY,
    BOOT_STABILIZE,
    BOOT_CALIBRATE,
    BOOT_MDNS,
    // Flight sequence
    PAD_IDLE,
    ASCENT,
    DESCENT,
    LANDED
} flight_state_t;

// Pyro firing modes
typedef enum { PYRO_MODE_FALLEN = 1, PYRO_MODE_AGL, PYRO_MODE_SPEED, PYRO_MODE_DELAY } pyro_mode_t;

// Flight sample
typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa;
    int32_t altitude_cm;
    uint8_t state;
    uint8_t under_thrust;
    uint8_t padding[2];
} flight_sample_t;

// Config
typedef struct {
    char id[9];
    char name[9];
    uint8_t pyro1_mode;     // pyro_mode_t
    uint16_t pyro1_value;   // in reporting units (cm, m, or ft depending on mode)
    uint8_t pyro2_mode;
    uint16_t pyro2_value;
    uint8_t units;          // 0=cm, 1=m, 2=ft
    uint8_t beep_mode;      // 0=digits, 1=hundreds
} config_t;

// Forward declarations  
typedef struct flight_context_t flight_context_t;

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
    int32_t vertical_speed_cms;  // cm/s, positive = up
    int32_t prev_vertical_speed_cms;
    uint32_t launch_time;        // backdated T=0
    uint32_t apogee_time;
    uint32_t last_sample;
    uint32_t last_telemetry;
    uint32_t landing_stable_since; // for landing detection
    bool pyro1_fired;
    bool pyro2_fired;
    bool pyro1_continuity_good;
    bool pyro2_continuity_good;
    bool pyros_armed;
    bool apogee_detected;
    bool under_thrust;
    uint16_t telemetry_seq;
    uint8_t pyro_firing;       // 0=none, 1=pyro1, 2=pyro2
    uint32_t pyro_fire_start;
    uint32_t pyro1_fire_time;  // for post-fire diagnostics
    uint32_t pyro2_fire_time;
    config_t config;
    flight_state_t current_state;
    int32_t filtered_pressure;
    bool filter_initialized;
    // Boot state fields
    uint32_t boot_timer;
    int cal_count;
    int32_t cal_sum;
} flight_context_t;

// State dispatch - defined after state functions
static flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now);

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

// Telemetry: $PYRO NMEA sentence
// Format: $PYRO,<seq>,<state>,<thrust>,<alt>,<vel>,<maxalt>,<press>,<time>,<flags>,<p1adc>,<p2adc>,<batt>,<temp>*<XX>\r\n
// All values integer, all units metric (cm, cm/s, Pa, ms, deci-°C).
// XOR checksum covers bytes between $ and * (exclusive).
void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state) {
    // Build flags byte
    uint8_t flags = 0;
    if (ctx->pyro1_continuity_good) flags |= (1 << 0);  // P1_CONT
    if (ctx->pyro2_continuity_good) flags |= (1 << 1);  // P2_CONT
    if (ctx->pyro1_fired)           flags |= (1 << 2);  // P1_FIRED
    if (ctx->pyro2_fired)           flags |= (1 << 3);  // P2_FIRED
    if (ctx->pyros_armed)           flags |= (1 << 4);  // ARMED
    if (ctx->apogee_detected)       flags |= (1 << 5);  // APOGEE

    // State: 0=PAD_IDLE, 1=ASCENT, 2=DESCENT, 3=LANDED
    uint8_t st = (uint8_t)state;

    // Thrust flag: 1 if ascending and speed still increasing
    uint8_t thrust = (state == ASCENT && ctx->under_thrust) ? 1 : 0;

    // Format the payload (everything between $ and *)
    char payload[160];
    snprintf(payload, sizeof(payload),
             "PYRO,%u,%u,%u,%ld,%ld,%ld,%ld,%lu,%02X,%u,%u,%u,%d",
             ctx->telemetry_seq,
             st,
             thrust,
             (long)altitude_cm,
             (long)ctx->vertical_speed_cms,
             (long)ctx->max_altitude,
             (long)ctx->filtered_pressure,
             (unsigned long)time_ms,
             flags,
             g_status.pyro1_adc,
             g_status.pyro2_adc,
             0,   // battery ADC (not yet implemented)
             0);  // temperature deci-°C (not yet implemented)

    // Compute XOR checksum over payload
    uint8_t checksum = 0;
    for (int i = 0; payload[i] != '\0'; i++) {
        checksum ^= (uint8_t)payload[i];
    }

    // Send complete sentence: $payload*XX\r\n
    char sentence[180];
    snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", payload, checksum);
    uart_puts(uart0, sentence);

    ctx->telemetry_seq++;
}

// Convert internal cm to reporting units
static int32_t cm_to_units(int32_t cm, uint8_t units) {
    switch (units) {
        case 1:  return cm / 100;           // meters
        case 2:  return cm * 100 / 3048;    // feet (integer approx)
        default: return cm;                 // cm
    }
}

// Check pyro firing condition (values in config units)
bool should_fire_pyro(flight_context_t *ctx, uint8_t mode, uint16_t value) {
    if (!ctx->apogee_detected) return false;
    int32_t fallen = cm_to_units(ctx->max_altitude - ctx->last_altitude, ctx->config.units);
    int32_t agl = cm_to_units(ctx->last_altitude, ctx->config.units);
    int32_t speed = cm_to_units(-ctx->vertical_speed_cms, ctx->config.units); // positive = downward, per second
    uint32_t delay_s = (to_ms_since_boot(get_absolute_time()) - ctx->apogee_time) / 1000;
    
    switch (mode) {
        case PYRO_MODE_FALLEN: return fallen >= value;
        case PYRO_MODE_AGL:    return agl <= (int32_t)value;
        case PYRO_MODE_SPEED:  return speed >= (int32_t)value;
        case PYRO_MODE_DELAY:  return delay_s >= value;
        default: return false;
    }
}

// ── Boot state functions ──

flight_state_t state_boot_filesystem(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    extern const struct lfs_config lfs_pico_flash_config;
    lfs_t lfs;
    int err = lfs_mount(&lfs, &lfs_pico_flash_config);
    if (err < 0) {
        lfs_format(&lfs, &lfs_pico_flash_config);
        lfs_mount(&lfs, &lfs_pico_flash_config);
    }
    uart_puts(uart0, "littlefs mounted\r\n");
    lfs_file_t config_file;
    err = lfs_file_open(&lfs, &config_file, "config.ini", LFS_O_RDONLY);
    if (err == LFS_ERR_NOENT) {
        err = lfs_file_open(&lfs, &config_file, "config.ini", LFS_O_WRONLY | LFS_O_CREAT);
        if (err == LFS_ERR_OK) {
            const char *config =
                "[pyro]\r\n"
                "id=PYRO001\r\n"
                "name=My Rocket\r\n"
                "pyro1_mode=delay\r\n"
                "pyro1_value=0\r\n"
                "pyro2_mode=agl\r\n"
                "pyro2_value=300\r\n"
                "units=m\r\n"
                "beep_mode=digits\r\n";
            lfs_file_write(&lfs, &config_file, config, strlen(config));
            lfs_file_close(&lfs, &config_file);
        }
    } else if (err == LFS_ERR_OK) {
        lfs_file_close(&lfs, &config_file);
    }
    lfs_unmount(&lfs);

    net_init();
    net_start();
    http_server_init();
    uart_puts(uart0, "network ready\r\n");

    pfb_firmware_commit();

    adc_gpio_init(26);
    adc_gpio_init(27);
    gpio_init(BUZZER);
    gpio_set_dir(BUZZER, GPIO_OUT);
    gpio_put(BUZZER, 0);

    uart_puts(uart0, "I2C init...\r\n");
    i2c_deinit(i2c1);

    ctx->boot_timer = to_ms_since_boot(get_absolute_time());
    return BOOT_I2C_SETTLE;
}

flight_state_t state_boot_i2c_settle(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->boot_timer >= 500) {
        return BOOT_SENSOR_DETECT;
    }
    return BOOT_I2C_SETTLE;
}

flight_state_t state_boot_sensor_detect(flight_context_t *ctx, uint32_t now) {
    (void)now;
    pressure_sensor_type_t sensor = pressure_sensor_init();
    if (sensor == PRESSURE_SENSOR_NONE) {
        uart_puts(uart0, "No pressure sensor!\r\n");
    } else {
        uart_puts(uart0, pressure_sensor_name());
        uart_puts(uart0, " detected\r\n");
    }
    return BOOT_PYRO_INIT;
}

flight_state_t state_boot_pyro_init(flight_context_t *ctx, uint32_t now) {
    (void)now;
    pyro_init();
    uart_puts(uart0, "pyro init done\r\n");
    return BOOT_CONTINUITY;
}

flight_state_t state_boot_continuity(flight_context_t *ctx, uint32_t now) {
    pyro_continuity_t cont1, cont2;
    pyro_check_continuity(&cont1, &cont2);
    ctx->pyro1_continuity_good = cont1.good;
    ctx->pyro2_continuity_good = cont2.good;
    uart_puts(uart0, "calibrating...\r\n");
    ctx->boot_timer = now;
    return BOOT_STABILIZE;
}

flight_state_t state_boot_stabilize(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->boot_timer >= 2000) {
        ctx->cal_count = 0;
        ctx->cal_sum = 0;
        ctx->boot_timer = now;
        return BOOT_CALIBRATE;
    }
    return BOOT_STABILIZE;
}

flight_state_t state_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->boot_timer >= 100) {
        ctx->boot_timer = now;
        pressure_reading_t data;
        pressure_sensor_read(&data);
        ctx->cal_sum += data.pressure_pa;
        ctx->cal_count++;
        if (ctx->cal_count >= 10) {
            ctx->ground_pressure = ctx->cal_sum / 10;
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "ground_pressure=%ld\r\n", (long)ctx->ground_pressure);
            uart_puts(uart0, dbg);
            return BOOT_MDNS;
        }
    }
    return BOOT_CALIBRATE;
}

flight_state_t state_boot_mdns(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    uart_puts(uart0, "starting mDNS...\r\n");
    net_mdns_poll();
    uart_puts(uart0, "mDNS started\r\n");
    uart_puts(uart0, "entering PAD_IDLE\r\n");
    return PAD_IDLE;
}

// ── Flight state functions ──

// Forward declarations
flight_state_t state_pad_idle(flight_context_t *ctx, uint32_t now);
flight_state_t state_ascent(flight_context_t *ctx, uint32_t now);
flight_state_t state_descent(flight_context_t *ctx, uint32_t now);
flight_state_t state_landed(flight_context_t *ctx, uint32_t now);

// State: PAD_IDLE
flight_state_t state_pad_idle(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 10) return PAD_IDLE;
    
    /* Re-check continuity every ~1 second while on pad */
    static uint32_t last_cont_check = 0;
    if (now - last_cont_check > 1000) {
        pyro_continuity_t c1, c2;
        pyro_check_continuity(&c1, &c2);
        ctx->pyro1_continuity_good = c1.good;
        ctx->pyro2_continuity_good = c2.good;
        g_status.pyro1_adc = c1.raw_adc;
        g_status.pyro2_adc = c2.raw_adc;
        last_cont_check = now;
    }

    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = (ctx->last_sample > 0) ? (now - ctx->last_sample) : 10;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    buf_add(ctx, 0, filtered, altitude, PAD_IDLE);
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    
    // Ascent detection: +10m (1000cm) in <1s
    if (altitude > 1000) {
        // Backdate T=0: scan buffer for first positive altitude
        ctx->launch_time = now;
        for (int i = ctx->buf_count - 1; i >= 0; i--) {
            uint16_t idx = (ctx->buf_head - 1 - i + 4096) % 4096;
            if (ctx->flight_buffer[idx].altitude_cm <= 50) { // ~0.5m threshold
                ctx->launch_time = now - (ctx->buf_count - 1 - i) * 10; // 10ms per sample
                break;
            }
        }
        return ASCENT;
    }
    return PAD_IDLE;
}

// State: ASCENT
flight_state_t state_ascent(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 100) return ASCENT;
    
    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    // Vertical speed
    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;
    
    // Thrust detection: speed increasing
    ctx->under_thrust = ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;
    
    uint32_t flight_time = now - ctx->launch_time;
    flight_sample_t *s = &ctx->flight_buffer[ctx->buf_head];
    buf_add(ctx, flight_time, filtered, altitude, ASCENT);
    // Set under_thrust on the sample we just added
    uint16_t last_idx = (ctx->buf_head - 1 + 4096) % 4096;
    ctx->flight_buffer[last_idx].under_thrust = ctx->under_thrust ? 1 : 0;
    
    if (altitude > ctx->max_altitude)
        ctx->max_altitude = altitude;
    
    // Arm pyros when speed < 10 m/s (1000 cm/s)
    if (!ctx->pyros_armed && ctx->vertical_speed_cms < 1000 && ctx->vertical_speed_cms >= 0)
        ctx->pyros_armed = true;
    
    // Apogee: armed AND altitude not increasing
    if (ctx->pyros_armed && !ctx->apogee_detected && ctx->vertical_speed_cms <= 0) {
        ctx->apogee_detected = true;
        ctx->apogee_time = now;
        ctx->apogee_protect_idx = (ctx->buf_head >= 50) ? (ctx->buf_head - 50) : (4096 + ctx->buf_head - 50);
        ctx->apogee_protected = true;
        
        // Check pyro firing at apogee
        if (!ctx->pyro1_fired && !pyro_is_firing() && ctx->pyro1_continuity_good &&
            should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
            pyro_fire(1);
            ctx->pyro1_fire_time = now;
        }
        if (!ctx->pyro2_fired && !pyro_is_firing() && ctx->pyro2_continuity_good &&
            should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value)) {
            pyro_fire(2);
            ctx->pyro2_fire_time = now;
        }
        
        ctx->last_altitude = altitude;
        ctx->last_sample = now;
        return DESCENT;
    }
    
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return ASCENT;
}

// State: DESCENT
flight_state_t state_descent(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 50) return DESCENT;
    
    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);
    
    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;
    
    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, DESCENT);
    
    // Check pyro firing
    if (!ctx->pyro1_fired && !pyro_is_firing() && ctx->pyro1_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
        pyro_fire(1);
        ctx->pyro1_fire_time = now;
    }
    if (!ctx->pyro2_fired && !pyro_is_firing() && ctx->pyro2_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value)) {
        pyro_fire(2);
        ctx->pyro2_fire_time = now;
    }
    
    /* Post-fire diagnostics: ~1s after fire, check if re-fire needed */
    if (ctx->pyro1_fired && ctx->pyro1_fire_time > 0 && now - ctx->pyro1_fire_time > 1000 && now - ctx->pyro1_fire_time < 1500) {
        bool ballistic = ctx->vertical_speed_cms < -3000;
        pyro_continuity_t c1, c2;
        pyro_check_continuity(&c1, &c2);
        if (ballistic && !c1.open) {
            pyro_fire(1);
            ctx->pyro1_fire_time = now;
        }
    }
    if (ctx->pyro2_fired && ctx->pyro2_fire_time > 0 && now - ctx->pyro2_fire_time > 1000 && now - ctx->pyro2_fire_time < 1500) {
        bool ballistic = ctx->vertical_speed_cms < -3000;
        pyro_continuity_t c1, c2;
        pyro_check_continuity(&c1, &c2);
        if (ballistic && !c2.open) {
            pyro_fire(2);
            ctx->pyro2_fire_time = now;
        }
    }
    
    // Landing detection: altitude change < 1m (100cm) for 1 second
    if (abs(altitude - ctx->last_altitude) < 100) {
        if (ctx->landing_stable_since == 0) ctx->landing_stable_since = now;
        if (now - ctx->landing_stable_since >= 1000) {
            ctx->last_altitude = altitude;
            ctx->last_sample = now;
            return LANDED;
        }
    } else {
        ctx->landing_stable_since = 0;
    }
    
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return DESCENT;
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

static flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now) {
    switch (ctx->current_state) {
        // Boot states
        case BOOT_FILESYSTEM:    return state_boot_filesystem(ctx, now);
        case BOOT_I2C_SETTLE:    return state_boot_i2c_settle(ctx, now);
        case BOOT_SENSOR_DETECT: return state_boot_sensor_detect(ctx, now);
        case BOOT_PYRO_INIT:     return state_boot_pyro_init(ctx, now);
        case BOOT_CONTINUITY:    return state_boot_continuity(ctx, now);
        case BOOT_STABILIZE:     return state_boot_stabilize(ctx, now);
        case BOOT_CALIBRATE:     return state_boot_calibrate(ctx, now);
        case BOOT_MDNS:          return state_boot_mdns(ctx, now);
        // Flight states
        case PAD_IDLE: return state_pad_idle(ctx, now);
        case ASCENT:   return state_ascent(ctx, now);
        case DESCENT:  return state_descent(ctx, now);
        case LANDED:   return state_landed(ctx, now);
        default:       return PAD_IDLE;
    }
}

int main() {
    board_init();
    net_mac_init();
    tud_init(BOARD_TUD_RHPORT);
    stdio_init_all();

    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    uart_puts(uart0, "UART initialized\r\n");
    uart_puts(uart0, "Pyro MK1B v" FW_VERSION " (" FW_BUILD_DATE ")\r\n");

    flight_context_t ctx = {0};
    ctx.config = (config_t){"PYRO001", "PYRO001", 1, 300, 1, 150};
    ctx.current_state = BOOT_FILESYSTEM;

    static uint32_t last_hb = 0;

    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        tud_task();
        net_service();

        /* Handle deferred picotool reset */
        extern volatile uint8_t pending_reset;
        if (pending_reset == 1) rom_reset_usb_boot(0, 0);
        if (pending_reset == 2) watchdog_reboot(0, 0, 100);

        /* Unified state machine: boot → flight */
        ctx.current_state = dispatch_state(&ctx, now);

        /* Update pyro (safe during boot — pyro_init hasn't run yet, pyro_update is a no-op) */
        pyro_update(now);

        /* Update shared status for HTTP dashboard */
        g_status.state = ctx.current_state;
        g_status.altitude_cm = ctx.last_altitude;
        g_status.max_altitude_cm = ctx.max_altitude;
        g_status.vertical_speed_cms = ctx.vertical_speed_cms;
        g_status.pressure_pa = ctx.filtered_pressure;
        g_status.pyro1_fired = ctx.pyro1_fired;
        g_status.pyro2_fired = ctx.pyro2_fired;
        g_status.pyros_armed = ctx.pyros_armed;
        g_status.pyro1_continuity = ctx.pyro1_continuity_good;
        g_status.pyro2_continuity = ctx.pyro2_continuity_good;
        g_status.under_thrust = ctx.under_thrust;
        g_status.flight_time_ms = (ctx.launch_time > 0) ? (now - ctx.launch_time) : 0;

        // $PYRO telemetry: 10Hz during ASCENT/DESCENT, 1Hz during PAD_IDLE/LANDED
        {
            uint32_t telem_interval = (ctx.current_state == ASCENT || ctx.current_state == DESCENT) ? 100 : 1000;
            if (now - ctx.last_telemetry >= telem_interval) {
                uint32_t flight_time = (ctx.current_state != PAD_IDLE) ? (now - ctx.launch_time) : 0;
                send_telemetry(&ctx, flight_time, ctx.last_altitude, ctx.current_state);
                ctx.last_telemetry = now;
            }
        }

        /* Heartbeat */
        static uint32_t last_hb;
        if (now - last_hb >= 1000) {
            last_hb = now;
            char hb[48];
            snprintf(hb, sizeof(hb), "[%lu] alive pa=%ld\r\n",
                     (unsigned long)now, (long)g_status.pressure_pa);
            uart_puts(uart0, hb);
        }
    }

    return 0;
}
