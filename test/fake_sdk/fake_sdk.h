/*
 * A fake of the Pico SDK calls a board file makes, so the board's own code
 * runs on the host (board_pyro_tests). Pins and ADC channels are arrays the
 * test sets and reads; time moves only when the test moves it.
 *
 * Every sleep and busy-wait fails the test that reaches it: the exec loop is
 * the only clock (DD-053).
 */
#ifndef FAKE_SDK_H
#define FAKE_SDK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned int uint;
typedef uint64_t absolute_time_t;

#define GPIO_OUT true
#define GPIO_IN false
#define FAKE_PINS 30

extern uint32_t fake_now_ms;
extern bool fake_level[FAKE_PINS];
extern bool fake_output[FAKE_PINS];
extern uint32_t fake_writes[FAKE_PINS]; /* gpio_put() calls, per pin */
extern uint32_t fake_rose_ms[FAKE_PINS]; /* when each pin last went high */
extern uint16_t fake_adc[4];            /* what each channel reads */
extern uint8_t fake_adc_selected;
extern void (*fake_on_adc_read)(uint8_t channel);
extern const char *fake_slept; /* the wait a test reached, or NULL */
extern uint8_t fake_func[FAKE_PINS];  /* each pin's function */
extern void (*fake_on_put)(uint pin, bool value);

#define GPIO_FUNC_I2C 3
#define GPIO_FUNC_SIO 5

static inline absolute_time_t get_absolute_time(void) {
    return (absolute_time_t)fake_now_ms * 1000u;
}
static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000u);
}

static inline void gpio_init(uint pin) {
    fake_output[pin] = false;
    fake_level[pin] = false;
    fake_func[pin] = GPIO_FUNC_SIO;
}
static inline void gpio_set_function(uint pin, uint fn) {
    fake_func[pin] = (uint8_t)fn;
}
static inline void gpio_set_dir(uint pin, bool out) {
    fake_output[pin] = out;
}
static inline void gpio_put(uint pin, bool value) {
    if (fake_on_put)
        fake_on_put(pin, value);
    if (value && !fake_level[pin])
        fake_rose_ms[pin] = fake_now_ms;
    fake_level[pin] = value;
    fake_writes[pin]++;
}
static inline bool gpio_get(uint pin) {
    return fake_level[pin];
}
static inline void gpio_pull_up(uint pin) {
    (void)pin;
}

static inline void adc_init(void) {}
static inline void adc_gpio_init(uint pin) {
    (void)pin;
}
static inline void adc_select_input(uint channel) {
    fake_adc_selected = (uint8_t)channel;
}
static inline uint16_t adc_read(void) {
    if (fake_on_adc_read)
        fake_on_adc_read(fake_adc_selected);
    return fake_adc[fake_adc_selected];
}

static inline uint64_t time_us_64(void) {
    return (uint64_t)fake_now_ms * 1000u;
}

/* Resets: the peripheral's reset handshake, which waits on hardware. */
#define RESETS_RESET_I2C0_BITS (1u << 3)
#define RESETS_RESET_I2C1_BITS (1u << 4)
static inline void reset_block(uint32_t bits) {
    (void)bits;
}
static inline void unreset_block_wait(uint32_t bits) {
    (void)bits;
}

/* I2C: test/fake_sdk/fake_i2c.c, with the devices a test attaches. */
typedef struct {
    int index;
} i2c_inst_t;
extern i2c_inst_t fake_i2c_inst[2];
#define i2c0 (&fake_i2c_inst[0])
#define i2c1 (&fake_i2c_inst[1])
#define PICO_ERROR_GENERIC (-1)
uint i2c_init(i2c_inst_t *i2c, uint baudrate);
uint i2c_set_baudrate(i2c_inst_t *i2c, uint baudrate);
int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop);
int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop);

static inline void sleep_ms(uint32_t ms) {
    (void)ms;
    fake_slept = "sleep_ms";
}
static inline void sleep_us(uint64_t us) {
    (void)us;
    fake_slept = "sleep_us";
}
static inline void busy_wait_us(uint64_t us) {
    (void)us;
    fake_slept = "busy_wait_us";
}

#endif /* FAKE_SDK_H */
