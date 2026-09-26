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

static inline absolute_time_t get_absolute_time(void) {
    return (absolute_time_t)fake_now_ms * 1000u;
}
static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000u);
}

static inline void gpio_init(uint pin) {
    fake_output[pin] = false;
    fake_level[pin] = false;
}
static inline void gpio_set_dir(uint pin, bool out) {
    fake_output[pin] = out;
}
static inline void gpio_put(uint pin, bool value) {
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
