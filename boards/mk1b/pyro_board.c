#include "pyro.h"
#include "pin_store.h"
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

/* ── Continuity [DD-053] ─────────────────────────────────────────
 *
 * PYRO_COMMON_EN on, with both enables off, is the stimulus. It is shared,
 * so one settle serves both channels. The sense node needs SETTLE_MS before
 * the ADC reads it, and the loop is that timer: each phase parks on a
 * deadline and a later pyro_update() picks it up. One reading a second, as
 * the flight code asks; pyro_get() returns the last completed one.
 *
 * The first pyro_update() starts it, never pyro_init(): the release claim
 * comes after init, and a board whose channels are both released never calls
 * pyro_update(), so its common, by then Lua's pad, is never left raised. */
#define SETTLE_MS 10
#define IDLE_MS (1000 - SETTLE_MS)

typedef enum { SNS_IDLE, SNS_SETTLING } sns_phase_t;

static uint8_t firing_channel;
static uint32_t fire_start_ms;

static sns_phase_t sns_phase;
static uint32_t sns_due_ms;
static bool sns_valid;
static pyro_continuity_t cont[2];

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
    sns_phase = SNS_IDLE;
    sns_due_ms = to_ms_since_boot(get_absolute_time());
    sns_valid = false;
}

static uint16_t adc_read_channel(uint8_t channel) {
    adc_select_input(channel);
    return adc_read(); /* raw 12-bit, 0-4095 */
}

static void classify(pyro_continuity_t *c) {
    c->open = c->raw_adc > ADC_OPEN_THRESHOLD;
    c->shorted = c->raw_adc < ADC_SHORT_THRESHOLD;
    c->good = !c->open && !c->shorted;
}

/* Only pads this board still owns. Driving the enables low is a precondition
 * for the measurement -- neither channel may be firing -- and on a RELEASED
 * channel that pad is a Lua output, where the same write would stamp it low on
 * every sample. The common is never released while either channel is
 * retained, so it needs no guard. */
static void sense_start(uint32_t now_ms) {
    if (pin_store_owns(PYRO1_EN)) {
        gpio_put(PYRO1_EN, 0);
    }
    if (pin_store_owns(PYRO2_EN)) {
        gpio_put(PYRO2_EN, 0);
    }
    gpio_put(PYRO_COMMON_EN, 1);
    sns_phase = SNS_SETTLING;
    sns_due_ms = now_ms + SETTLE_MS;
}

static void sense_service(uint32_t now_ms) {
    if ((int32_t)(now_ms - sns_due_ms) < 0)
        return;
    if (sns_phase == SNS_IDLE) {
        sense_start(now_ms);
        return;
    }
    cont[0].raw_adc = adc_read_channel(PYRO1_ADC_CH);
    cont[1].raw_adc = adc_read_channel(PYRO2_ADC_CH);
    gpio_put(PYRO_COMMON_EN, 0);
    classify(&cont[0]);
    classify(&cont[1]);
    sns_valid = true;
    sns_phase = SNS_IDLE;
    sns_due_ms = now_ms + IDLE_MS;
}

/* The cycle runs from pyro_update(); there is no stimulus to start here. */
void pyro_sample(void) {}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel != 1 && channel != 2)
        return;
    if (!sns_valid) {
        /* No completed reading yet: not-good, rather than a zeroed struct. */
        out->raw_adc = 0;
        out->good = false;
        out->open = true;
        out->shorted = false;
        return;
    }
    *out = cont[channel - 1];
}

void pyro_fire(uint8_t channel) {
    gpio_put(PYRO_COMMON_EN, 1);
    gpio_put(channel == 1 ? PYRO1_EN : PYRO2_EN, 1);
    firing_channel = channel;
    fire_start_ms = to_ms_since_boot(get_absolute_time());
}

void pyro_update(uint32_t now_ms) {
    if (firing_channel) {
        /* The cycle waits while firing: the pulse holds the common. */
        if (now_ms - fire_start_ms >= FIRE_DURATION_MS) {
            gpio_put(firing_channel == 1 ? PYRO1_EN : PYRO2_EN, 0);
            firing_channel = 0;
            /* The common stays on as the stimulus, so a fresh reading lands a
             * settle after the pulse, inside the post-fire verify window that
             * opens as it ends. */
            sense_start(now_ms);
        }
        return;
    }
    sense_service(now_ms);
}

bool pyro_is_firing(void) {
    return firing_channel != 0;
}

bool pyro_fault(uint8_t channel) {
    /* AP2192 FLAG is active-low: LOW = overcurrent/thermal fault */
    return !gpio_get(channel == 1 ? PYRO1_FLAG_PIN : PYRO2_FLAG_PIN);
}
