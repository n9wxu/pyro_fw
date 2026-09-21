/*
 * Pyro backend — Pyro MK1C.
 *
 * Implements src/pyro.h for the MK1C firing architecture: a TPS259570
 * eFuse armed by a software charge pump, bias-injection continuity
 * sensing, and four divided analog sense channels.
 *
 * Specifications live with the board design, not in this repo:
 *   ~/Documents/pyro_mk1c/DESIGN.md            hardware, levels, FMEA, invariants
 *   ~/Documents/pyro_mk1c/IGNITER_OPERATION.md S0-S8 and F0-F10 state machines
 *
 * ===========================================================================
 * STAGE: sense only (plan phase 3). Firing is NOT implemented.
 * ===========================================================================
 *
 * Implemented here: T1 (quiescent read) and T2 (bus-bias tracking test).
 * Both run with U9 off and the firing bus at 0 V.
 *
 * NOT implemented yet: T3 (per-channel bias, which localises a shorted
 * low-side FET and identifies a shorted TVS), the arm pump, precharge, the
 * pad test and F0-F10. T3 arrives with the pad test, because its readings
 * are only unambiguous in that sequence's ordering -- a connected match and
 * a shorted TVS both read ~1037 counts, and only measuring with the match
 * isolated separates them.
 *
 * ---------------------------------------------------------------------------
 * SAFETY INVARIANTS (DESIGN.md 9, plus 13a/13b added for MK1C)
 *
 *   13a. FIRE_A and FIRE_B are asserted ONLY by the F0-F10 firing sequence,
 *        on a real fire command from a valid authority. No diagnostic, test,
 *        self-check, boot path or operator command asserts either, ever.
 *   13b. ARM_TOGGLE is asserted ONLY by the F0-F10 firing sequence or by the
 *        optional bus precharge test, behind its full interlock.
 *
 * At this stage all three signals are driven low by pyro_safe_all_outputs()
 * and never written again. There is deliberately no code path that raises
 * them, which is what makes T2 and T3 safe to run continuously.
 *
 *    5.  ARM_TOGGLE must be software-generated, from the code path that has
 *        just re-checked every arm condition. NEVER from a hardware timer,
 *        a PWM slice, a PIO program or a DMA pacer: a peripheral keeps
 *        running after the firmware stops, and would keep a dead processor
 *        armed.
 *
 * DESIGN.md 9 invariants 8 and 9 govern the fault latch below: require N
 * consecutive agreeing samples before latching, and never apply outlier
 * rejection when CLEARING a fault.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "board_if.h"
#include "board_pins.h"
#include "hal.h"
#include "version.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "hardware/structs/watchdog.h"
#include "arm_pump.pio.h"
#include "pico/stdlib.h"
#include <stdio.h>

extern void hal_telemetry_send(const char *sentence);

/* ── Sense scaling ────────────────────────────────────────────────── */

/* All four dividers are 4.99k/(10k+4.99k) = 49.9k/(100k+49.9k) = 0.3329.
 * At 12 bits against a 3.3 V reference:
 *     3300mV / 4095 counts / 0.3329 = 2.421 mV of NODE voltage per count,
 * i.e. 413 counts per volt at the node.
 *
 * This reproduces every level in DESIGN.md 4 and is the single most
 * error-prone constant in the port -- see the anchors asserted below. */
#define NODE_UV_PER_COUNT 2421

uint32_t pyro_counts_to_node_mv(uint16_t counts) {
    return ((uint32_t)counts * NODE_UV_PER_COUNT) / 1000u;
}

/* Anchors from DESIGN.md 4. If a divider value or the reference changes,
 * these fail at compile time rather than silently shifting every threshold. */
_Static_assert((1058u * NODE_UV_PER_COUNT) / 1000u >= 2550 && (1058u * NODE_UV_PER_COUNT) / 1000u <= 2570,
               "bus bias, no match: 1058 counts should be ~2.56 V");
_Static_assert((1214u * NODE_UV_PER_COUNT) / 1000u >= 2930 && (1214u * NODE_UV_PER_COUNT) / 1000u <= 2950,
               "channel bias, match off: 1214 counts should be ~2.94 V");
_Static_assert((1037u * NODE_UV_PER_COUNT) / 1000u >= 2500 && (1037u * NODE_UV_PER_COUNT) / 1000u <= 2520,
               "shorted TVS / match present: 1037 counts should be ~2.51 V");
_Static_assert((3469u * NODE_UV_PER_COUNT) / 1000u >= 8390 && (3469u * NODE_UV_PER_COUNT) / 1000u <= 8410,
               "full 2S bus: 3469 counts should be ~8.4 V");

/* ── Expected levels, in ADC counts (DESIGN.md 4) ─────────────────── */

#define CNT_QUIESCENT_MAX 50   /* a cold, unbiased node                     */
#define CNT_TRACK_PRESENT 400  /* S3 present/open split; ~10:1 margin either side */
#define CNT_BUS_BIASED 1058    /* bus under its own bias, no match          */
#define CNT_CH_ISOLATED 1214   /* channel under its own bias, match off     */
#define CNT_CH_LOADED 1037     /* channel tied to the bus pull-down         */

/* ── Tracking test timing (DESIGN.md S3) ─────────────────────────── */

#define TRACK_BIAS_MS 8     /* 5-10 ms of bias per measurement */
#define TRACK_PERIOD_MS 500 /* duty-cycled: one test per few hundred ms */

/* ── Fault latch ──────────────────────────────────────────────────── */

#define FAULT_CONFIRM_N 3 /* invariant 8: N consecutive agreeing samples */

typedef enum {
    PF_NONE = 0,
    PF_BUS_HOT = 1u << 0,      /* bus at pack voltage with the pump stopped */
    PF_BUS_SHORT_GND = 1u << 1 /* bus will not rise under its own bias      */
} pyro_fault_bits_t;

static uint8_t fault_latch;    /* pyro_fault_bits_t, latched (invariant 4) */
static uint8_t bus_hot_run;    /* consecutive agreeing samples */
static uint8_t bus_short_run;  /* consecutive agreeing samples */

/* ── Sense state ──────────────────────────────────────────────────── */

static uint16_t sns_vbat, sns_bus, sns_a, sns_b; /* latest quiescent (T1) */
static uint16_t trk_bus, trk_a, trk_b;           /* latest tracking (T2)  */
static bool trk_valid;
static uint32_t next_track_ms;
static uint32_t last_report_ms;

static uint16_t adc_sample(uint8_t channel) {
    adc_select_input(channel);
    return (uint16_t)adc_read(); /* raw 12-bit, 0-4095 */
}

/* Median of 3, per DESIGN.md 7.1 step 3. */
static uint16_t adc_median3(uint8_t channel) {
    uint16_t x = adc_sample(channel);
    uint16_t y = adc_sample(channel);
    uint16_t z = adc_sample(channel);
    if (x > y) {
        uint16_t t = x;
        x = y;
        y = t;
    }
    if (y > z)
        y = (x > z) ? x : z;
    return y;
}

/* ── Output safing ────────────────────────────────────────────────── */

/* Drive every pyro output inactive. Called from board_early_init() before
 * any slow initialisation, and again from pyro_init().
 *
 * This is the ONLY function in the tree that writes FIRE_A, FIRE_B or
 * ARM_TOGGLE at this stage, and it only ever writes them low. */
void pyro_safe_all_outputs(void) {
    static const uint8_t outputs[] = {
        BOARD_PIN_ARM_TOGGLE, BOARD_PIN_FIRE_A, BOARD_PIN_FIRE_B,
        BOARD_PIN_BIAS_A,     BOARD_PIN_BIAS_B, BOARD_PIN_BIAS_BUS,
    };
    for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        gpio_init(outputs[i]);
        gpio_put(outputs[i], 0); /* set the level before the direction */
        gpio_set_dir(outputs[i], GPIO_OUT);
        gpio_put(outputs[i], 0);
    }
}

/* ── T1: quiescent read ───────────────────────────────────────────
 *
 * Applies no stimulus at all: every bias GPIO stays low and nothing is
 * energised. Free, so it runs on every update. */
static void t1_quiescent(void) {
    sns_vbat = adc_median3(BOARD_ADC_CH_VBAT);
    sns_bus = adc_median3(BOARD_ADC_CH_BUS);
    sns_a = adc_median3(BOARD_ADC_CH_A);
    sns_b = adc_median3(BOARD_ADC_CH_B);
}

/* ── T2: bus-bias tracking test (DESIGN.md S3) ────────────────────
 *
 * Asserts BIAS_BUS only; BIAS_A and BIAS_B stay low. Bridgewire current is
 * about 0.2 mA, 500x below a 100 mA no-fire current. The bus is never
 * armed, so no firing energy exists regardless of the result.
 *
 * A channel that follows the bus (~1030 counts) has a match across it; one
 * that stays near zero (<50) is open or absent. The raw counts are what get
 * reported, not the boolean: a dirty connector at 5 kohm reads ~190 counts
 * and would pass a naive threshold. */
static void t2_tracking(void) {
    gpio_put(BOARD_PIN_BIAS_BUS, 1);
    sleep_ms(TRACK_BIAS_MS);

    trk_bus = adc_median3(BOARD_ADC_CH_BUS);
    trk_a = adc_median3(BOARD_ADC_CH_A);
    trk_b = adc_median3(BOARD_ADC_CH_B);

    gpio_put(BOARD_PIN_BIAS_BUS, 0);
    trk_valid = true;
}

/* BRING-UP ONLY. T3 puts about 1.4 mA through a connected match -- 7x the
 * routine tracking current of T2, though still 70x below a 100 mA no-fire
 * current. DESIGN.md S3 specifies BIAS_BUS only for routine presence
 * tracking, so T3 does not belong in the flight loop: it moves into the
 * operator-commanded pad test (plan phase 4.4), where its readings are also
 * unambiguous because the match is isolated at that point.
 *
 * Set to 0 before flight. Left on now because it is what localises a fault
 * to a channel during board bring-up. */
#ifndef PYRO_MK1C_BRINGUP_T3
#define PYRO_MK1C_BRINGUP_T3 1
#endif

/* BENCH MODE. Holds BIAS_BUS continuously high so the bus reaches a steady
 * DC level a multimeter can read, instead of the ~3% duty pulse train the
 * routine tracking test uses. Suppresses every other probe so nothing
 * disturbs the node.
 *
 * Still completely cold on the firing side: FIRE_A, FIRE_B and ARM_TOGGLE
 * are untouched (invariants 13a/13b), U9 is off, and the bus can only reach
 * the ~1.7 V the bias divider allows. Bridgewire current is about 0.9 mA,
 * over 100x below a no-fire current.
 *
 * Build with -DPYRO_MK1C_BIAS_HOLD=1. Must be 0 for flight. */
#ifndef PYRO_MK1C_BIAS_HOLD
#define PYRO_MK1C_BIAS_HOLD 0
#endif

/* ── Bring-up probe: bus decay time constant ──────────────────────
 *
 * A charge-time-constant probe lived here too, deriving R120 from
 * tau_decay/tau_charge. It was WRONG: it reported 221 us where bench DC
 * measurements imply 428 us, so its derived R120 (526 ohm) disagreed with
 * the measured 1375 ohm by 2.6x. Removed rather than left reporting a
 * number that looks authoritative. tau_decay itself is consistent with the
 * bench results and is kept.
 *
 *
 * The bus level under bias is set by the DIVIDER Rpd/(R120+Rpd), so a wrong
 * R120 and a wrong R103 produce an identical reading. The decay after the
 * bias is released depends on the pull-down ALONE:
 *
 *     tau = Rpd * C_bus
 *
 * With only C115 (1uF) fitted:
 *     R103 wrong, Rpd ~405 ohm   -> tau ~405 us
 *     R120 wrong, Rpd ~1918 ohm  -> tau ~1918 us
 *
 * A 4.7x separation, well clear of the 33 us ADC filter. If a THT C_BULK is
 * populated both scale together, so the absolute value only settles the
 * question when C_bus is known. */
static uint16_t decay_tau_us;

#if PYRO_MK1C_BRINGUP_T3
static void bus_decay_probe(void) {
    gpio_put(BOARD_PIN_BIAS_BUS, 1);
    sleep_ms(TRACK_BIAS_MS);
    adc_select_input(BOARD_ADC_CH_BUS);
    uint16_t peak = (uint16_t)adc_read();
    if (peak < 100) { /* nothing to decay from */
        gpio_put(BOARD_PIN_BIAS_BUS, 0);
        decay_tau_us = 0;
        return;
    }
    uint16_t thresh = (uint16_t)(((uint32_t)peak * 37u) / 100u); /* 1/e */

    absolute_time_t t0 = get_absolute_time();
    gpio_put(BOARD_PIN_BIAS_BUS, 0);
    uint32_t us = 0;
    for (int i = 0; i < 4000; i++) {
        if ((uint16_t)adc_read() <= thresh) {
            us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
            break;
        }
    }
    decay_tau_us = (us > 65535u) ? 65535u : (uint16_t)us;
}
#else
static void bus_decay_probe(void) {
    decay_tau_us = 0;
}
#endif

/* T3 results, declared here because the capture header reports them. */
static uint16_t bias_a_counts, bias_b_counts;

/* ── High-speed waveform capture ──────────────────────────────────
 *
 * Streams the ADC FIFO into RAM by DMA so the full charge and discharge
 * curves can be fitted off-board, rather than inferring a time constant
 * from a single threshold crossing. The threshold approach that lived here
 * before was wrong by 1.94x and nothing in the firmware could tell.
 *
 * The edge is placed WAVE_PRE_US into the capture so there is a baseline to
 * fit against. Sample interval is set per mode, because the charge constant
 * is several times shorter than the decay one.
 *
 * Bus-cold throughout: only BIAS_BUS moves. FIRE_A, FIRE_B and ARM_TOGGLE
 * are untouched (invariants 13a/13b). */
#define WAVE_N 2048
#define WAVE_PRE_US 200

static uint16_t wave_buf[WAVE_N];
static uint16_t wave_dt_us;
static uint16_t wave_pre_n;
static bool wave_valid;

/* ADC clock is 48 MHz; one conversion is 96 cycles, so the divider counts
 * in units of 1/48us. div = 48 * dt_us - 1 gives a sample every dt_us. */
static void wave_capture(bool charge, uint16_t dt_us) {
    wave_dt_us = dt_us;
    wave_pre_n = (uint16_t)(WAVE_PRE_US / dt_us);

    /* Establish the starting condition and let it settle fully. */
    gpio_put(BOARD_PIN_BIAS_BUS, charge ? 0 : 1);
    sleep_ms(30);

    adc_select_input(BOARD_ADC_CH_BUS);
    adc_fifo_drain();
    adc_fifo_setup(true,  /* enable FIFO            */
                   true,  /* DMA data request       */
                   1,     /* DREQ on 1 sample       */
                   false, /* no error bit in FIFO   */
                   false  /* keep 12-bit samples    */
    );
    adc_set_clkdiv((float)(48.0 * (double)dt_us) - 1.0f);

    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure(ch, &c, wave_buf, &adc_hw->fifo, WAVE_N, true);

    adc_run(true);
    busy_wait_us(WAVE_PRE_US);
    gpio_put(BOARD_PIN_BIAS_BUS, charge ? 1 : 0); /* the edge */

    dma_channel_wait_for_finish_blocking(ch);
    adc_run(false);
    adc_fifo_drain();
    dma_channel_unclaim(ch);

    /* Leave the bus cold. */
    gpio_put(BOARD_PIN_BIAS_BUS, 0);

    /* Restore one-shot ADC use for the routine tests. */
    adc_fifo_setup(false, false, 0, false, false);
    adc_set_clkdiv(0);
    wave_valid = true;
}

/* ── ARM interlock (DESIGN.md S1 interlock, plan phase 4.4) ───────
 *
 * The ONLY gate on asserting ARM_TOGGLE outside the firing sequence
 * (invariant 13b). Every condition must hold; the reason for a refusal is
 * reported so a failed interlock is never silent.
 *
 * Note what the channel checks are for. T2 reading open says no match is in
 * circuit. T3 reading ~1214 says the bias injector on that channel actually
 * works, which is what makes the T2 reading trustworthy: an open injector
 * would make a live, firable channel read open too. T3 also proves neither
 * low-side FET is shorted, which is the single failure that would make an
 * energised bus dangerous. */
#define ARM_UVLO_COUNTS 400 /* ~0.97 V at the pack; below this, do not arm */

static const char *arm_interlock_refusal(void) {
    if (fault_latch != PF_NONE)
        return "latched fault";
    if (!trk_valid)
        return "no tracking result yet";
    if (trk_a >= CNT_TRACK_PRESENT || trk_b >= CNT_TRACK_PRESENT)
        return "a match is present -- disconnect it";
    if (bias_a_counts < 1000 || bias_b_counts < 1000)
        return "channel bias low: shorted FET, drain short, or open injector";
    if (sns_vbat < ARM_UVLO_COUNTS)
        return "pack below UVLO";
    if (sns_bus > CNT_QUIESCENT_MAX)
        return "bus is not cold";
    return NULL; /* clear to arm */
}

/* Re-evaluated on every pump cycle. Invariant 5: the toggle must be a
 * byproduct of the safety checks, so that failing a check stops the pump by
 * construction rather than by a separate action. */
static inline bool arm_conditions_still_ok(void) {
    return fault_latch == PF_NONE;
}

/* ── ARM_TOGGLE pump (PIO, FIFO-paced) ───────────────────────────── */

#define ARM_PIO pio0
#define ARM_PUMP_PERIOD_US 100 /* 10 kHz, bottom of the DESIGN.md 5.1 band */
#define ARM_PUMP_BURST 10      /* toggle cycles per pushed word = 1 ms      */
#define ARM_WATCHDOG_MS 50     /* only armed while pumping                  */

static uint arm_sm = 0;
static uint arm_offset = 0;
static bool arm_pio_loaded = false;

static void arm_pump_start(void) {
    if (!arm_pio_loaded) {
        arm_offset = pio_add_program(ARM_PIO, &arm_pump_program);
        arm_sm = (uint)pio_claim_unused_sm(ARM_PIO, true);
        arm_pio_loaded = true;
    }
    /* 125 MHz / (50 clocks per period / period_us * 1e6) */
    float clkdiv = (float)clock_get_hz(clk_sys) / (50.0f * (1000000.0f / (float)ARM_PUMP_PERIOD_US));
    arm_pump_program_init(ARM_PIO, arm_sm, arm_offset, BOARD_PIN_ARM_TOGGLE, clkdiv);
}

static void arm_pump_stop(void) {
    pio_sm_set_enabled(ARM_PIO, arm_sm, false);
    pio_sm_clear_fifos(ARM_PIO, arm_sm);
    /* Hand the pad back to SIO and drive it low, so nothing can toggle it. */
    gpio_init(BOARD_PIN_ARM_TOGGLE);
    gpio_put(BOARD_PIN_ARM_TOGGLE, 0);
    gpio_set_dir(BOARD_PIN_ARM_TOGGLE, GPIO_OUT);
    gpio_put(BOARD_PIN_ARM_TOGGLE, 0);
}

/* The SDK has no watchdog_disable(); setting a long load and not feeding it
 * is not the same thing. Push the deadline far out so the scoped window does
 * not reboot us later, and leave normal operation as it was. */
static void watchdog_disable_after_arm(void) {
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

/* ── Arm / precharge capture ──────────────────────────────────────
 *
 * Runs the charge pump so U9 turns on and the bus ramps at the dVdT slew
 * rate, captures the ramp, then STOPS pumping and captures the passive
 * disarm and decay in the same window.
 *
 * FIRE_A and FIRE_B are never written here (invariant 13a). With no match
 * connected, both low-side switches open and the mechanical disconnect out,
 * there is no circuit through any bridgewire regardless of bus voltage. */
static void wave_capture_arm(uint16_t dt_us, uint32_t pump_ms) {
    wave_dt_us = dt_us;
    wave_pre_n = (uint16_t)(WAVE_PRE_US / dt_us);

    gpio_put(BOARD_PIN_BIAS_BUS, 0); /* bias off: it would corrupt the ramp */
    sleep_ms(20);

    adc_select_input(BOARD_ADC_CH_BUS);
    adc_fifo_drain();
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv((float)(48.0 * (double)dt_us) - 1.0f);

    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure(ch, &c, wave_buf, &adc_hw->fifo, WAVE_N, true);

    adc_run(true);
    busy_wait_us(WAVE_PRE_US);

    /* ── Pump ──
     *
     * FIFO-paced PIO: each pushed word buys ARM_PUMP_BURST toggle cycles and
     * then the state machine stalls. Two independent things stop it:
     *
     *   1. The CPU stops pushing -- because it hung, crashed, or an arm
     *      condition went false. The FIFO drains and the SM stalls.
     *   2. The watchdog fires. PSM_WDSEL resets the PIO block, which stops
     *      the SM and returns the pad to input with a pull-down.
     *
     * The watchdog is enabled only for the armed window. hal_platform_init
     * deliberately leaves it off in normal operation, and a short timeout
     * there caused boot loops, so it is scoped to exactly the interval where
     * a wedged processor would otherwise hold the bus up. */
    arm_pump_start();
    watchdog_enable(ARM_WATCHDOG_MS, true);

    const uint32_t bursts = (pump_ms * 1000u) / (ARM_PUMP_BURST * ARM_PUMP_PERIOD_US);
    for (uint32_t i = 0; i < bursts; i++) {
        if (!arm_conditions_still_ok())
            break; /* stopping the toggle IS the disarm */
        watchdog_update();
        /* Blocks only while the FIFO is full, which bounds how far ahead the
         * pump can ever run of the checks above. */
        pio_sm_put_blocking(ARM_PIO, arm_sm, ARM_PUMP_BURST - 1);
    }

    arm_pump_stop(); /* pump stopped -> C_HOLD bleeds -> U9 disarms */
    watchdog_disable_after_arm();

    /* The rest of the window captures the EN collapse and the bus decay. */
    dma_channel_wait_for_finish_blocking(ch);
    adc_run(false);
    adc_fifo_drain();
    dma_channel_unclaim(ch);

    adc_fifo_setup(false, false, 0, false, false);
    adc_set_clkdiv(0);
    wave_valid = true;
}

/* Deferred request, serviced from pyro_update() in the main loop. */
static volatile int wave_state;    /* 0 idle, 1 busy, 2 ready */
static volatile bool wave_pending;
static volatile int wave_pending_mode; /* 0 charge, 1 decay, 2 arm */
static const char *wave_refusal;

bool pyro_wave_request(int mode) {
    if (wave_state == 1)
        return false; /* one at a time */
    if (mode == 2) {
        wave_refusal = arm_interlock_refusal();
        if (wave_refusal) {
            hal_telemetry_send("!PYRO ARM REFUSED by interlock\r\n");
            return false;
        }
    }
    wave_pending_mode = mode;
    wave_pending = true;
    wave_state = 1;
    return true;
}

const char *pyro_wave_refusal(void) {
    return wave_refusal ? wave_refusal : "";
}

int pyro_wave_state(void) {
    return wave_state;
}

/* Stream the capture out as CSV. Written incrementally so no multi-kB
 * buffer is needed: 2048 rows is about 20 kB of text. */
static void wave_write_csv(int mode) {
    const bool charge = (mode == 0);
    const char *path = (mode == 2) ? "/wave_a.csv" : (charge ? "/wave_c.csv" : "/wave_d.csv");
    hal_file_t *f = hal_fs_open(path, false);
    if (!f) {
        wave_state = 0;
        return;
    }
    /* Self-describing header: everything the analysis needs, so the host
     * tool hardcodes no constants and a capture file stays interpretable on
     * its own. Comment lines start with '#', then a normal CSV header row. */
    char line[128];
    int n;

#define WCSV(...)                                                                                                      \
    do {                                                                                                               \
        n = snprintf(line, sizeof(line), __VA_ARGS__);                                                                 \
        hal_fs_write(f, line, n);                                                                                      \
    } while (0)

    WCSV("# capture,pyro bus %s\n", (mode == 2) ? "arm" : (charge ? "charge" : "decay"));
    WCSV("# stimulus_detail,%s\n",
         (mode == 2) ? "ARM_TOGGLE pumped 10kHz for 10ms then stopped" : "BIAS_BUS step");
    WCSV("# design_slew_mv_per_ms,890\n");
    WCSV("# board,%s\n", BOARD_NAME_STR);
    WCSV("# fw_version,%s\n", FW_VERSION);
    WCSV("# node,FIRING_BUS\n");
    WCSV("# stimulus,BIAS_BUS\n");
    /* --- timebase --- */
    WCSV("# dt_us,%u\n", (unsigned)wave_dt_us);
    WCSV("# sample_hz,%lu\n", (unsigned long)(1000000u / (unsigned)wave_dt_us));
    WCSV("# n_samples,%u\n", (unsigned)WAVE_N);
    WCSV("# pre_n,%u\n", (unsigned)wave_pre_n);
    WCSV("# edge_us,%u\n", (unsigned)WAVE_PRE_US);
    /* --- ADC scaling: node_uV = counts * uv_per_count --- */
    WCSV("# adc_bits,12\n");
    WCSV("# adc_vref_mv,3300\n");
    WCSV("# divider_num_ohm,4990\n");
    WCSV("# divider_den_ohm,14990\n");
    WCSV("# uv_per_count,%u\n", (unsigned)NODE_UV_PER_COUNT);
    /* --- design values, for the host tool to compare against --- */
    WCSV("# design_r_bias_ohm,330\n");
    WCSV("# design_r_bleed_ohm,2200\n");
    WCSV("# design_r_div_ohm,14990\n");
    WCSV("# design_rpd_ohm,1918\n");
    WCSV("# design_c_bus_nf,1000\n");
    WCSV("# design_bus_biased_counts,1058\n");
    WCSV("# design_ch_biased_counts,1214\n");
    /* --- levels measured by the routine tests, same session --- */
    WCSV("# meas_bus_quiescent_counts,%u\n", (unsigned)sns_bus);
    WCSV("# meas_bus_biased_counts,%u\n", (unsigned)(trk_valid ? trk_bus : 0));
    WCSV("# meas_ch_a_biased_counts,%u\n", (unsigned)bias_a_counts);
    WCSV("# meas_ch_b_biased_counts,%u\n", (unsigned)bias_b_counts);
    WCSV("# meas_vbat_counts,%u\n", (unsigned)sns_vbat);
    WCSV("i,counts\n");
#undef WCSV

    for (uint16_t i = 0; i < WAVE_N; i++) {
        n = snprintf(line, sizeof(line), "%u,%u\n", (unsigned)i, (unsigned)wave_buf[i]);
        hal_fs_write(f, line, n);
    }
    hal_fs_close(f);
    wave_state = 2;
}

static void wave_service(void) {
    if (!wave_pending)
        return;
    wave_pending = false;
    int mode = wave_pending_mode;

    if (mode == 2) {
        /* 12 us x 2048 = 24.6 ms: enough for a ~5 ms ramp at the dVdT slew
         * rate plus the disarm and decay after the pump stops. */
        wave_capture_arm(12u, 10u);
    } else {
        /* Charge constant is the shorter of the two, so sample it faster. */
        wave_capture(mode == 0, mode == 0 ? 4u : 12u);
    }
    if (wave_valid)
        wave_write_csv(mode);
    else
        wave_state = 0;
}

/* ── T3: per-channel bias ─────────────────────────────────────────
 *
 * Asserts BIAS_A alone, then BIAS_B alone, with the bus cold and FIRE_x
 * never touched. About 1.4 mA, 70x below a 100 mA no-fire current.
 *
 * With the match disconnected the channel node is isolated from the bus, so
 * it floats up to 3.0 x 14.99k/(330 + 14.99k) = 2.94 V (1214 counts). The
 * divider values and the bias resistor are identical to the bus network, so
 * comparing this against the bus reading separates a weak bias source
 * (common to all three) from a wrong value in the bus network alone. */
#if PYRO_MK1C_BRINGUP_T3
static void t3_channel_bias(void) {
    gpio_put(BOARD_PIN_BIAS_A, 1);
    sleep_ms(TRACK_BIAS_MS);
    bias_a_counts = adc_median3(BOARD_ADC_CH_A);
    gpio_put(BOARD_PIN_BIAS_A, 0);

    sleep_ms(2);

    gpio_put(BOARD_PIN_BIAS_B, 1);
    sleep_ms(TRACK_BIAS_MS);
    bias_b_counts = adc_median3(BOARD_ADC_CH_B);
    gpio_put(BOARD_PIN_BIAS_B, 0);
}
#else
static void t3_channel_bias(void) {
    bias_a_counts = bias_b_counts = 0;
}
#endif

/* ── Fault evaluation (DESIGN.md 8.1) ─────────────────────────────
 *
 * The firmware does not classify faults; it bounds-checks each measurement
 * and latches. Only the two rows of 8.1 that are safety decisions rather
 * than diagnoses are acted on here. Working out WHY a reading is out of
 * band is bench work, done from the logged raw counts. */
static void evaluate_faults(void) {
    /* Bus sitting at pack voltage with the pump stopped: the high side is
     * shorted to the battery. Do not arm. */
    bool hot = (sns_vbat > 200u) && (sns_bus > (uint16_t)(sns_vbat - (sns_vbat / 4u)));
    bus_hot_run = hot ? (uint8_t)(bus_hot_run + 1) : 0;
    if (bus_hot_run >= FAULT_CONFIRM_N) {
        bus_hot_run = FAULT_CONFIRM_N;
        fault_latch |= PF_BUS_HOT;
    }

    /* Bus will not rise under its own bias: shorted to ground, or a fitted
     * match is shorting to ground through a channel. */
    if (trk_valid) {
        bool shorted = trk_bus < CNT_QUIESCENT_MAX;
        bus_short_run = shorted ? (uint8_t)(bus_short_run + 1) : 0;
        if (bus_short_run >= FAULT_CONFIRM_N) {
            bus_short_run = FAULT_CONFIRM_N;
            fault_latch |= PF_BUS_SHORT_GND;
        }
    }
    /* Faults latch. Nothing clears them but a reset (invariants 4 and 6). */
}

/* Raw sense readout for /api/status, via board_pyro_raw() in hal_board.c.
 * Reports the bus level measured DURING the bus-bias pulse (T2), which is
 * the number that says whether the bias injector and the bleed network are
 * behaving: ~1058 counts healthy, ~1214 with R_BLEED open, <50 shorted. */
bool pyro_raw_sense(board_pyro_raw_t *out) {
    out->bus_quiescent = sns_bus;                 /* T1: no stimulus  */
    out->bus_biased = trk_valid ? trk_bus : 0;    /* T2: bus bias     */
    out->ch_a_biased = bias_a_counts;             /* T3: channel bias */
    out->ch_b_biased = bias_b_counts;
    out->vbat = sns_vbat;
    out->bus_decay_tau_us = decay_tau_us;
    return true;
}

/* ── pyro.h implementation ────────────────────────────────────────── */

void pyro_init(void) {
    pyro_safe_all_outputs();

    adc_init();
    adc_gpio_init(BOARD_PIN_SNS_VBAT);
    adc_gpio_init(BOARD_PIN_SNS_BUS);
    adc_gpio_init(BOARD_PIN_SNS_A);
    adc_gpio_init(BOARD_PIN_SNS_B);

    fault_latch = PF_NONE;
    bus_hot_run = bus_short_run = 0;
    trk_valid = false;
    next_track_ms = 0;
    last_report_ms = 0;

    t1_quiescent();
    hal_telemetry_send("!PYRO MK1C sense-only build: firing not implemented\r\n");
}

void pyro_check_continuity(pyro_continuity_t *p1, pyro_continuity_t *p2) {
    /* Returns the most recent duty-cycled tracking result rather than
     * blocking. Every caller already tolerates a slightly stale reading --
     * the flight-side continuity poll runs at 1 Hz. */
    uint16_t a = trk_valid ? trk_a : 0;
    uint16_t b = trk_valid ? trk_b : 0;

    p1->raw_adc = a;
    p1->open = a < CNT_TRACK_PRESENT;
    p1->good = !p1->open;
    p1->shorted = false; /* needs T3; not attributed from the bus-bias test */

    p2->raw_adc = b;
    p2->open = b < CNT_TRACK_PRESENT;
    p2->good = !p2->open;
    p2->shorted = false;
}

void pyro_fire(uint8_t channel) {
    /* Not implemented. Asserting FIRE_x is reserved to the F0-F10 sequence
     * (invariant 13a), which does not exist in this build. Refuse loudly
     * rather than silently doing nothing. */
    (void)channel;
    hal_telemetry_send("!PYRO FIRE REFUSED: firing not implemented on MK1C\r\n");
}

void pyro_update(uint32_t now_ms) {
    /* Serviced here, in the main loop, so the flash write stays out of the
     * network callback path (DECISIONS.md #2). */
    wave_service();

    t1_quiescent();

#if PYRO_MK1C_BIAS_HOLD
    /* Bench mode: hold the bias on and run no other probe. */
    gpio_put(BOARD_PIN_BIAS_BUS, 1);
    trk_bus = adc_median3(BOARD_ADC_CH_BUS);
    trk_a = adc_median3(BOARD_ADC_CH_A);
    trk_b = adc_median3(BOARD_ADC_CH_B);
    trk_valid = true;
    if ((int32_t)(now_ms - last_report_ms) < 2000)
        return;
    last_report_ms = now_ms;
    {
        char l[96];
        snprintf(l, sizeof(l), "!PYRO BIAS HOLD bus=%u a=%u b=%u\r\n", trk_bus, trk_a, trk_b);
        hal_telemetry_send(l);
    }
    return;
#endif

    if ((int32_t)(now_ms - next_track_ms) >= 0) {
        next_track_ms = now_ms + TRACK_PERIOD_MS;
        t2_tracking();
        t3_channel_bias();
        bus_decay_probe();
    }

    evaluate_faults();

    if ((int32_t)(now_ms - last_report_ms) < 5000)
        return;
    last_report_ms = now_ms;

    char line[160];
    snprintf(line, sizeof(line),
             "!PYRO q[vbat=%u bus=%u a=%u b=%u] trk[bus=%u a=%u b=%u] vbat=%lumV flt=0x%02x\r\n", sns_vbat, sns_bus,
             sns_a, sns_b, trk_bus, trk_a, trk_b, (unsigned long)pyro_counts_to_node_mv(sns_vbat),
             (unsigned)fault_latch);
    hal_telemetry_send(line);
}

bool pyro_is_firing(void) {
    return false;
}

bool pyro_fault(uint8_t channel) {
    /* U9's ~FLT is not routed to the MCU on MK1C. Once the firing sequence
     * exists this also reports the latched F5 classification. For now it
     * reports the bus-level latches, which apply to both channels. */
    (void)channel;
    return fault_latch != PF_NONE;
}
