#include "pyro.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"

#define PYRO_COMMON_EN 15
#define PYRO1_EN       21
#define PYRO2_EN       22
#define PYRO1_ADC_CH   0   /* GPIO 26 */
#define PYRO2_ADC_CH   1   /* GPIO 27 */

#define FIRE_DURATION_MS 500

/* Thresholds (raw 12-bit ADC, 0-4095) */
#define ADC_OPEN_THRESHOLD   3800   /* above = open circuit */
#define ADC_SHORT_THRESHOLD  50     /* below = dead short */

static uint8_t  firing_channel;
static uint32_t fire_start_ms;

void pyro_init(void) {
    adc_init();
    adc_gpio_init(26);
    adc_gpio_init(27);

    gpio_init(PYRO_COMMON_EN);
    gpio_init(PYRO1_EN);
    gpio_init(PYRO2_EN);
    gpio_set_dir(PYRO_COMMON_EN, GPIO_OUT);
    gpio_set_dir(PYRO1_EN, GPIO_OUT);
    gpio_set_dir(PYRO2_EN, GPIO_OUT);
    gpio_put(PYRO_COMMON_EN, 0);
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);

    firing_channel = 0;
}

static uint16_t adc_read_channel(uint8_t channel) {
    adc_select_input(channel);
    return adc_read();  /* raw 12-bit, 0-4095 */
}

void pyro_check_continuity(pyro_continuity_t *p1, pyro_continuity_t *p2) {
    gpio_put(PYRO1_EN, 0);
    gpio_put(PYRO2_EN, 0);
    gpio_put(PYRO_COMMON_EN, 1);
    sleep_ms(10);

    p1->raw_adc = adc_read_channel(PYRO1_ADC_CH);
    p2->raw_adc = adc_read_channel(PYRO2_ADC_CH);

    gpio_put(PYRO_COMMON_EN, 0);

    p1->open    = p1->raw_adc > ADC_OPEN_THRESHOLD;
    p1->shorted = p1->raw_adc < ADC_SHORT_THRESHOLD;
    p1->good    = !p1->open && !p1->shorted;

    p2->open    = p2->raw_adc > ADC_OPEN_THRESHOLD;
    p2->shorted = p2->raw_adc < ADC_SHORT_THRESHOLD;
    p2->good    = !p2->open && !p2->shorted;
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
