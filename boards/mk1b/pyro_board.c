#include "pyro.h"
#include "board_pins.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"

#define PYRO_COMMON_EN BOARD_PIN_PYRO_COMMON_EN
#define PYRO1_EN BOARD_PIN_PYRO1_EN
#define PYRO2_EN BOARD_PIN_PYRO2_EN
#define PYRO1_ADC_CH 0 /* GPIO 26 */
#define PYRO2_ADC_CH 1 /* GPIO 27 */
#define PYRO1_FLAG_PIN BOARD_PIN_PYRO1_FLAG
#define PYRO2_FLAG_PIN BOARD_PIN_PYRO2_FLAG

#define FIRE_DURATION_MS 500

/* Thresholds (raw 12-bit ADC, 0-4095) */
#define ADC_OPEN_THRESHOLD 3800 /* above = open circuit */
#define ADC_SHORT_THRESHOLD 50  /* below = dead short */

static uint8_t firing_channel;
static uint32_t fire_start_ms;

void pyro_init(void) {
    adc_init();
    adc_gpio_init(BOARD_PIN_PYRO1_SENSE);
    adc_gpio_init(BOARD_PIN_PYRO2_SENSE);

    gpio_init(PYRO_COMMON_EN);
    gpio_init(PYRO1_EN);
    gpio_init(PYRO2_EN);
    gpio_set_dir(PYRO_COMMON_EN, GPIO_OUT);
    gpio_set_dir(PYRO1_EN, GPIO_OUT);
    gpio_set_dir(PYRO2_EN, GPIO_OUT);
    gpio_put(PYRO_COMMON_EN, 0);
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);

    /* AP2192 FLAG pins: active-low open-drain, need pull-up */
    gpio_init(PYRO1_FLAG_PIN);
    gpio_set_dir(PYRO1_FLAG_PIN, GPIO_IN);
    gpio_pull_up(PYRO1_FLAG_PIN);
    gpio_init(PYRO2_FLAG_PIN);
    gpio_set_dir(PYRO2_FLAG_PIN, GPIO_IN);
    gpio_pull_up(PYRO2_FLAG_PIN);

    firing_channel = 0;
}

static uint16_t adc_read_channel(uint8_t channel) {
    adc_select_input(channel);
    return adc_read(); /* raw 12-bit, 0-4095 */
}

/* Latched by pyro_sample(), read back by pyro_get(). */
static pyro_continuity_t cont[2];

static void classify(pyro_continuity_t *c) {
    c->open = c->raw_adc > ADC_OPEN_THRESHOLD;
    c->shorted = c->raw_adc < ADC_SHORT_THRESHOLD;
    c->good = !c->open && !c->shorted;
}

/* One stimulus event for BOTH channels: PYRO_COMMON_EN is shared, so the
 * 10 ms settle and the bridgewire current are paid once regardless of how
 * many channels the caller goes on to read. */
void pyro_sample(void) {
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);
    gpio_put(PYRO_COMMON_EN, 1);
    sleep_ms(10);

    cont[0].raw_adc = adc_read_channel(PYRO1_ADC_CH);
    cont[1].raw_adc = adc_read_channel(PYRO2_ADC_CH);

    gpio_put(PYRO_COMMON_EN, 0);

    classify(&cont[0]);
    classify(&cont[1]);
}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel == 1 || channel == 2)
        *out = cont[channel - 1];
}

void pyro_fire(uint8_t channel) {
    gpio_put(PYRO_COMMON_EN, 1);
    gpio_put(channel == 1 ? PYRO1_EN : PYRO2_EN, 1);
    firing_channel = channel;
    fire_start_ms = to_ms_since_boot(get_absolute_time());
}

void pyro_update(uint32_t now_ms) {
    if (firing_channel && (now_ms - fire_start_ms >= FIRE_DURATION_MS)) {
        gpio_put(firing_channel == 1 ? PYRO1_EN : PYRO2_EN, 0);
        gpio_put(PYRO_COMMON_EN, 0);
        firing_channel = 0;
    }
}

bool pyro_is_firing(void) {
    return firing_channel != 0;
}

bool pyro_fault(uint8_t channel) {
    /* AP2192 FLAG is active-low: LOW = overcurrent/thermal fault */
    return !gpio_get(channel == 1 ? PYRO1_FLAG_PIN : PYRO2_FLAG_PIN);
}
