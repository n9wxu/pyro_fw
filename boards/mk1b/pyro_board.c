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

/* ── Thresholds, in raw 12-bit counts [DD-059] ────────────────────
 *
 * The sense node is MK1A's (sim/plant/plant_mk1b.c, from the netlist): 100k
 * to 3V3, and PYRO_COMMON_EN the shared low-side gate. With the common on, an
 * igniter pulls the node to 0 counts, a 1k bad joint to 41, a 10k leak to
 * 372; open reads 4095. With it off, only a short to ground pulls it down. */
#define CNT_PATH_MAX 500  /* below: a conducting path to ground exists */
#define CNT_OPEN_MIN 3000 /* above: no path at all                     */

/* ── Continuity [DD-053, DD-059] ─────────────────────────────────
 *
 * Two readings a second, as MK1A takes them. With the common off, the node
 * has had the idle second to charge, so a channel still low is shorted to
 * ground. Then PYRO_COMMON_EN on, with both enables off, is the stimulus,
 * and a channel pulled low has its igniter. The common is shared, so one
 * settle serves both. The loop is the timer: each phase parks on a deadline
 * and a later pyro_update() picks it up; pyro_get() returns the last
 * completed reading.
 *
 * The first pyro_update() starts it, never pyro_init(): the release claim
 * comes after init, and a board whose channels are both released never calls
 * pyro_update(), so its common, by then Lua's pad, is never left raised. */
#define SETTLE_MS 10
#define IDLE_MS (1000 - SETTLE_MS)
#define RECHARGE_MS 50 /* 5 x 100.1k x 100 nF, from power-on or the common */

typedef enum { SNS_IDLE, SNS_SETTLING } sns_phase_t;

static uint8_t firing_channel;
static uint32_t fire_start_ms;

static sns_phase_t sns_phase;
static uint32_t sns_due_ms;
static bool sns_valid;
static pyro_continuity_t cont[2];
static uint16_t idle_counts[2]; /* the common off: low is a short */

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
    sns_due_ms = to_ms_since_boot(get_absolute_time()) + RECHARGE_MS;
    sns_valid = false;
    idle_counts[0] = idle_counts[1] = 4095;
}

static uint16_t adc_read_channel(uint8_t channel) {
    adc_select_input(channel);
    return adc_read(); /* raw 12-bit, 0-4095 */
}

/* The raw count is the stimulated one: a degraded joint sits between the
 * thresholds, and only the number shows it. */
static void classify(pyro_continuity_t *c, uint16_t idle) {
    c->shorted = idle < CNT_PATH_MAX;
    c->open = c->raw_adc > CNT_OPEN_MIN;
    c->good = c->raw_adc < CNT_PATH_MAX && !c->shorted;
}

/* Only pads this board still owns. Driving the enables low is a precondition
 * for the measurement -- neither channel may be firing -- and on a RELEASED
 * channel that pad is a Lua output, where the same write would stamp it low on
 * every sample. The common is never released while either channel is
 * retained, so it needs no guard. */
static void sense_start(uint32_t now_ms, bool read_idle) {
    if (read_idle) {
        idle_counts[0] = adc_read_channel(PYRO1_ADC_CH);
        idle_counts[1] = adc_read_channel(PYRO2_ADC_CH);
    }
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
        sense_start(now_ms, true);
        return;
    }
    cont[0].raw_adc = adc_read_channel(PYRO1_ADC_CH);
    cont[1].raw_adc = adc_read_channel(PYRO2_ADC_CH);
    gpio_put(PYRO_COMMON_EN, 0);
    classify(&cont[0], idle_counts[0]);
    classify(&cont[1], idle_counts[1]);
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
             * opens as it ends. With the common on there is no short reading:
             * the last one stands. */
            sense_start(now_ms, false);
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
