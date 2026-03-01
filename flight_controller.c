#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "pressure_sensor.h"

// GPIO Pins
#define PYRO_COMMON_EN 15
#define PYRO1_EN 21
#define PYRO2_EN 22
#define PYRO1_FAULT 17
#define PYRO2_FAULT 18
#define PYRO1_CONT_ADC 0
#define PYRO2_CONT_ADC 1

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

// Globals
static flight_sample_t flight_buffer[4096];
static uint16_t buf_head = 0, buf_tail = 0, buf_count = 0;
static uint16_t apogee_protect_idx = 0;
static bool apogee_protected = false;
static flight_state_t state = PAD_IDLE;
static int32_t ground_pressure = 0;
static int32_t max_altitude = 0;
static uint32_t launch_time = 0;
static bool pyro1_fired = false, pyro2_fired = false;
static config_t config = {"PYRO001", "PYRO001", 1, 300, 1, 150};

// Continuity check
uint16_t adc_oversample(uint8_t channel, uint16_t samples) {
    uint32_t sum = 0;
    for (uint16_t i = 0; i < samples; i++) {
        adc_select_input(channel);
        sum += adc_read();
    }
    return sum / samples;
}

void check_continuity(uint16_t *pyro1, uint16_t *pyro2) {
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);
    gpio_put(PYRO_COMMON_EN, 0);
    sleep_ms(10);
    gpio_put(PYRO_COMMON_EN, 1);
    sleep_ms(10);
    *pyro1 = adc_oversample(PYRO1_CONT_ADC, 256);
    *pyro2 = adc_oversample(PYRO2_CONT_ADC, 256);
    gpio_put(PYRO_COMMON_EN, 0);
}

// Buffer operations
void buf_add(uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    if (buf_count == 4096) {
        if (apogee_protected && buf_tail == apogee_protect_idx) return;
        buf_tail = (buf_tail + 1) % 4096;
        buf_count--;
    }
    flight_buffer[buf_head].time_ms = time_ms;
    flight_buffer[buf_head].pressure_pa = pressure;
    flight_buffer[buf_head].altitude_cm = altitude;
    flight_buffer[buf_head].state = st;
    buf_head = (buf_head + 1) % 4096;
    buf_count++;
}

// Telemetry
void send_telemetry(uint32_t time_ms, int32_t altitude_cm) {
    char buf[128];
    int len = 0;
    
    len += snprintf(buf + len, sizeof(buf) - len, "{%03ld>", altitude_cm / 3048);
    len += snprintf(buf + len, sizeof(buf) - len, "@%d>", state == PAD_IDLE ? 1 : state == LAUNCH ? 2 : state == APOGEE ? 5 : state == FALLING ? 8 : 9);
    len += snprintf(buf + len, sizeof(buf) - len, "#%04lu>", time_ms / 1000);
    
    if (state == PAD_IDLE) {
        len += snprintf(buf + len, sizeof(buf) - len, "~AB---->");
    } else {
        len += snprintf(buf + len, sizeof(buf) - len, "~%c%c---->", pyro1_fired ? '1' : 'A', pyro2_fired ? '2' : 'B');
    }
    
    if (state >= APOGEE) {
        len += snprintf(buf + len, sizeof(buf) - len, "%%0%04ld>", max_altitude / 30);
    }
    
    len += snprintf(buf + len, sizeof(buf) - len, "=%s>", config.name);
    
    uart_puts(uart0, buf);
}

// Fire pyro
void fire_pyro(uint8_t channel) {
    gpio_put(PYRO_COMMON_EN, 1);
    gpio_put(channel == 1 ? PYRO1_EN : PYRO2_EN, 1);
    sleep_ms(500);
    gpio_put(channel == 1 ? PYRO1_EN : PYRO2_EN, 0);
    gpio_put(PYRO_COMMON_EN, 0);
    if (channel == 1) pyro1_fired = true;
    else pyro2_fired = true;
}

int main() {
    stdio_init_all();
    
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
    gpio_set_dir(PYRO_COMMON_EN, GPIO_OUT);
    gpio_set_dir(PYRO1_EN, GPIO_OUT);
    gpio_set_dir(PYRO2_EN, GPIO_OUT);
    gpio_put(PYRO_COMMON_EN, 0);
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);
    
    // Init pressure sensor
    if (pressure_sensor_init(i2c0) != 0) {
        while (1) sleep_ms(1000);
    }
    
    // Continuity check
    uint16_t cont1, cont2;
    check_continuity(&cont1, &cont2);
    
    // Calibrate ground pressure
    int32_t sum = 0;
    for (int i = 0; i < 10; i++) {
        pressure_data_t data;
        pressure_sensor_read(&data);
        sum += data.pressure_pa;
        sleep_ms(100);
    }
    ground_pressure = sum / 10;
    
    uint32_t last_sample = 0;
    int32_t last_altitude = 0;
    uint32_t last_telemetry = 0;
    
    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        pressure_data_t pdata;
        pressure_sensor_read(&pdata);
        int32_t altitude = pressure_to_altitude_cm(pdata.pressure_pa, ground_pressure);
        
        // State machine
        uint32_t sample_interval = (state == FALLING) ? 50 : (state == PAD_IDLE || state == LANDED) ? 1000 : 100;
        
        if (now - last_sample >= sample_interval) {
            uint32_t flight_time = (state >= LAUNCH) ? (now - launch_time) : 0;
            buf_add(flight_time, pdata.pressure_pa, altitude, state);
            
            if (state == PAD_IDLE && altitude > 1000) {
                state = LAUNCH;
                launch_time = now;
            } else if (state == LAUNCH && altitude > max_altitude) {
                max_altitude = altitude;
            } else if (state == LAUNCH && altitude < max_altitude - 1000) {
                state = APOGEE;
                apogee_protect_idx = (buf_head >= 50) ? (buf_head - 50) : (4096 + buf_head - 50);
                apogee_protected = true;
            } else if (state == APOGEE) {
                state = FALLING;
            } else if (state == FALLING) {
                int32_t fallen = (max_altitude - altitude) / 100;
                int32_t agl = altitude / 100;
                
                if (!pyro1_fired && ((config.pyro1_mode == 1 && fallen >= config.pyro1_distance) || 
                                     (config.pyro1_mode == 2 && agl <= config.pyro1_distance))) {
                    fire_pyro(1);
                }
                if (!pyro2_fired && ((config.pyro2_mode == 1 && fallen >= config.pyro2_distance) || 
                                     (config.pyro2_mode == 2 && agl <= config.pyro2_distance))) {
                    fire_pyro(2);
                }
                
                if (altitude < 500 && abs(altitude - last_altitude) < 100) {
                    state = LANDED;
                }
            }
            
            last_altitude = altitude;
            last_sample = now;
        }
        
        // Telemetry at 1Hz
        if (now - last_telemetry >= 1000) {
            send_telemetry(now - launch_time, altitude);
            last_telemetry = now;
        }
        
        sleep_ms(10);
    }
    
    return 0;
}
