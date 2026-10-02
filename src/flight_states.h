#ifndef FLIGHT_STATES_H
#define FLIGHT_STATES_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal.h"
#include "ground_test.h"     /* ground_test_ctx_t embedded in flight_context_t */
#include "ground_test_seq.h" /* gt_seq_t, likewise */

/* What the power-up test and the pad check found [FLT-BOOT-15]. */
#define DIAG_SENSOR_FAIL (1u << 0)
#define DIAG_FS_FAIL (1u << 1)
#define DIAG_CFG_RANGE (1u << 2) /* a pyro altitude setting is beyond the sensor */
#define DIAG_P1_OPEN (1u << 3)
#define DIAG_P1_SHORT (1u << 4)
#define DIAG_P2_OPEN (1u << 5)
#define DIAG_P2_SHORT (1u << 6)
#define DIAG_BROWNOUT (1u << 7)     /* came back mid-flight after a power event */
#define DIAG_SENSOR_STUCK (1u << 8) /* a full window of identical readings [SNS-PRES-10] */
#define DIAG_SENSOR_LOST (1u << 9)  /* no sample for 0.5 s in flight [SNS-PRES-11] */

/* The pyro ones can be fixed at the rocket; the rest mean safe it and walk
 * away. The split picks the beep. */
#define DIAG_PYRO_ANY (DIAG_P1_OPEN | DIAG_P1_SHORT | DIAG_P2_OPEN | DIAG_P2_SHORT)
#define DIAG_FATAL_ANY (DIAG_SENSOR_FAIL | DIAG_FS_FAIL | DIAG_CFG_RANGE)
/* Re-derived at every pad check [PYR-CONT-03]. */
#define DIAG_PAD_ANY (DIAG_PYRO_ANY | DIAG_CFG_RANGE)

/* The name a DIAG_* bit goes by on /api/status and the console. */
const char *flight_diag_name(uint16_t bit);

/* See docs/flight_states.md "Per state". */
typedef enum {
    BOOT_SETTLE = 0,
    BOOT_CONTINUITY,
    BOOT_CALIBRATE,
    PAD_IDLE,
    ASCENT,
    FALLING,        /* no canopy yet: free fall or a failed one */
    DROGUE_DESCENT, /* settled at a drogue's rate */
    CHUTE_DESCENT,  /* settled at a main's rate */
    LANDED,
    /* Appended: state numbers are in every recorded flight log. */
    BOOT_SENSOR,
    FAULT,       /* terminal: the board cannot fly and says so */
    GROUND_TEST, /* terminal: powered up with the ground test pin asserted [GND-TEST-05] */
    STATE_COUNT
} flight_state_t;

typedef enum {
    SEVT_NONE = 0,
    SEVT_DONE,
    SEVT_TIMER,
    SEVT_CAL_DONE,
    SEVT_LAUNCH,
    SEVT_ARMED,
    SEVT_APOGEE,
    /* What the rocket is doing, not which pyro was commanded [DD-023]. */
    SEVT_DROGUE,   /* descent has steadied at a drogue-like rate */
    SEVT_CHUTE,    /* descent has steadied at a main-like rate */
    SEVT_FREEFALL, /* a canopy that was working has stopped working */
    SEVT_LANDING,
    SEVT_FAULT, /* a power-up test failed; nothing recovers from this */
    /* [FLT-BROWN-02] Back from a power event in flight. */
    SEVT_RECOVER_ASCENT,
    SEVT_RECOVER_DESCENT,
    SEVT_GROUND_TEST, /* the pin was held through power-up [GND-TEST-05] */
} state_event_t;

struct flight_context_t;

typedef state_event_t (*detect_fn)(struct flight_context_t *ctx, uint32_t now);
typedef void (*action_fn)(struct flight_context_t *ctx, uint32_t now);

/* [DD-053] The sensor is brought up a step a loop from boot, in about a
 * quarter of a second; one still going after this is a missing sensor. */
#define SENSOR_PENDING 0xFFu
#define SENSOR_BRINGUP_MS 5000u

typedef struct {
    flight_state_t from;
    state_event_t event;
    flight_state_t to;
    action_fn action;
} transition_t;

#include "flight_events.h"

typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa;
    int32_t altitude_cm;
    uint8_t state;
    uint8_t under_thrust;
    uint8_t event; /* EVT_*; EVT_NONE for a plain sample */
} flight_sample_t;

/* The last 64 samples, for flight_save_csv(). The flight record is hal_log's
 * flight_log.bin; nothing on the flight path reads this back. */
#define FLIGHT_BUF_SIZE 64

typedef struct flight_context_t {
    flight_sample_t flight_buffer[FLIGHT_BUF_SIZE];
    uint16_t buf_head;
    uint16_t buf_tail;
    uint16_t buf_count;
    int32_t ground_pressure;
    int32_t max_altitude;
    int32_t last_altitude;
    int32_t last_height; /* unclamped: the speed short of a fit [SNS-ALT-04] */
    int32_t vertical_speed_cms;
    int32_t prev_vertical_speed_cms;
    float air_scale; /* pp_air_scale() at the last descent sample; 0 is 1 */
    uint32_t launch_time;
    uint32_t apogee_time;
    uint32_t last_sample;
    uint32_t last_telemetry;
    uint32_t landing_stable_since;
    uint32_t still_since; /* the landing timeout's, as held() keeps it [FLT-LAND-07] */
    bool pyro1_fired;
    bool pyro2_fired;
    bool pyro1_continuity_good;
    bool pyro2_continuity_good;
    bool pyros_armed;
    bool apogee_detected;
    bool under_thrust;
    uint16_t telemetry_seq;
    uint16_t pyro1_adc;
    uint16_t pyro2_adc;
    uint32_t pyro1_fire_time; /* last command, fired or refused */
    uint32_t pyro2_fire_time;
    /* [PYR-FIRE-01] The board took the fire call and energised nothing. */
    bool pyro1_refused;
    bool pyro2_refused;
    bool pyro1_fault; /* [PYR-FAULT-02] */
    bool pyro2_fault;
    bool pyro1_verify_fail; /* post-fire continuity still good (pyro didn't open) */
    bool pyro2_verify_fail;
    config_t config;
    flight_state_t current_state;
    int32_t filtered_pressure;
    uint32_t boot_timer;
    uint32_t last_cont_check;
    bool buzzer_started;
    /* [FLT-LAUNCH-03] The first sample above 50 cm since the last one at or
     * below it: T+0, once the detector trips a hundred feet later. */
    bool pad_rising;
    uint32_t pad_rise_ms;
    /* [FLT-LAUNCH-07, FLT-APO-01] When each trigger's condition first held,
     * in sample time; 0 while it does not. */
    uint32_t launch_held_since;
    uint32_t apogee_held_since;
    uint32_t landed_row_ms;      /* [FLT-RATE-03] the last 1 Hz LANDED row, in sample time */
    int32_t max_speed_cms;       /* the arming gate's [DD-017] */
    uint32_t armed_time;         /* the record that arming preceded apogee [DD-022] */
    uint32_t descent_start_time; /* the landing timeout's start [FLT-LAND-07] */
    uint32_t landing_time;       /* flight time stops here [WEB-UI-04] */
    int32_t pad_speed_cms;
    uint8_t last_reason; /* beep_reason_t last said, for BEEP STATUS [GND-TEST-01] */

    /* What hal_pressure_sensor() said: 0 none answered, SENSOR_PENDING still
     * being brought up. */
    uint8_t sensor_type;
    bool fs_ok;

    /* ── Brownout recovery [FLT-BROWN-01..05] ───────────────────── */
    uint8_t reset_cause;
    uint8_t recovery;
    uint8_t recovery_why; /* cold_reason_t, when recovery is RECOVER_COLD */
    bool marker_written;
    bool marker_spent; /* invalidated at LANDED [FLT-BROWN-04] */

    /* [USB-01..04, USB-08] Test mode is RAM only, so off at every boot. */
    bool usb_attached;
    bool test_mode;

    uint16_t diag; /* DIAG_* bits: what is wrong; the beep is what to do */

    /* ── Mach lockout [FLT-MACH-02..07, DD-049] ────────────────────
     * Times are sample times. p_flag_pa: the fitted pressure at the flag. */
    bool mach_lock;
    bool mach_released; /* released once: the flag now needs a clean fit */
    bool mach_fallback; /* the fallback declared apogee */
    bool arm_height;    /* p < 0.9965 p0 has been seen */
    bool peak_lower_bound;
    int32_t p_flag_pa;
    uint32_t mach_flag_ms; /* 0: never flagged */
    uint32_t mach_release_ms;
    uint32_t release_since; /* ts + 1 form, as held() keeps it */
    uint32_t fallback_since;

    /* ── Descent phase [FLT-DESC-01] ──────────────────────────────
     * desc_ref_cms: the rate when the current dwell opened. */
    uint8_t desc_band;
    uint32_t desc_band_since;
    int32_t desc_ref_cms;
    uint32_t desc_fail_since;

    /* ── Emergency ladder [FLT-EMRG-01..04, PYR-REFIRE-01] ──────── */
    uint8_t pyro1_refires;
    bool main_forced; /* the ladder overrode pyro2's configured trigger */
    uint32_t emrg_fail_since;
    int32_t emrg_fail_ref_cms;
    /* ── The fit [DD-048] ─────────────────────────────────────────
     * What the triggers read of the newest sample's fit. The trigger height
     * is the fit's, unclamped, or the filtered height short of a fit. The
     * peak is the lowest pressure a clean fit showed in ASCENT; 0: none. */
    int32_t trigger_height_cm;
    bool fit_clean;
    bool fit_suspect; /* a gap or a stuck run in the window [SNS-PRES-10/11] */
    float fit_pa;     /* this sample's fitted pressure */
    /* The last clean fit, for what a charge can and cannot do [PYR-MODE-06]. */
    float clean_pa, clean_pdot, clean_pddot;
    uint32_t clean_ms; /* 0: none yet */
    bool sensor_stuck;
    bool sensor_lost;
    /* Sample time + 1 of the first unclean fit in a run, and of the first
     * clean one since; 0: none. A run ends only when clean has lasted the
     * fit's window: a lone clean fit in a swinging canopy does not end it. */
    uint32_t unclean_since;
    uint32_t clean_since;
    float p_min_pa;
    int32_t peak_height_cm;
    uint32_t apogee_fit_ms; /* where the fit put the apogee, in sample time */
    bool pyro1_due;         /* its trigger was met; fired once the other's pulse ends */
    bool pyro2_due;

    ground_test_ctx_t gt; /* [GND-TEST-01..04, DD-011] */

    /* [GND-TEST-05..11] The ground test pin at power-up, and the procedure
     * it asks for. */
    bool gt_held;
    uint32_t gt_held_since;
    bool gt_requested;
    gt_seq_t gt_seq;
} flight_context_t;

void flight_init(flight_context_t *ctx);
flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now);
void flight_update_outputs(flight_context_t *ctx, uint32_t now);
/* The flash writes the flight software owes. Call only where a write can
 * land: inside the hardware's flash window, and every tick on the host. */
void flight_flash_service(flight_context_t *ctx, uint32_t now);
/* [DAT-07] The ring buffer's last 64 samples as flight.csv, for the
 * simulator. */
int flight_save_csv(flight_context_t *ctx);

/* Milliseconds since launch while airborne, frozen at the landing, and 0 on
 * the pad or in any state with no flight behind it [WEB-UI-04]. */
uint32_t flight_elapsed_ms(const flight_context_t *ctx, uint32_t now);

/* What brownout recovery made of this boot, for /api/status. */
const char *flight_recovery_text(const flight_context_t *ctx);

/* [PYR-DEPLOY-02, PYR-FIRE-01] Energise one channel, if the shared element is
 * free: the one place a channel is fired from, flight or ground test, so the
 * interlock sits below every caller. */
typedef enum { PYRO_BUSY, PYRO_REFUSED, PYRO_ENERGISED } pyro_fire_result_t;
pyro_fire_result_t flight_pyro_energise(uint8_t channel);

/* telemetry_formatter.c: a $PYRO sentence for this context. */
void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state);

void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st);

/* ── Config reload: on the pad only ──────────────────────────────── */

typedef enum {
    CFG_APPLIED = 0,
    CFG_NOT_ON_PAD = -1,
    CFG_LOAD_FAILED = -2,
    CFG_INVALID = -3,
} cfg_apply_t;

cfg_apply_t flight_config_reload(flight_context_t *ctx);

/* The apply half, for a config already loaded: on the hardware the net task
 * loads it and the flight task applies it (rtos_tasks.h, flight_call()). */
cfg_apply_t flight_config_apply(flight_context_t *ctx, const config_t *new_config);

/* NULL before flight_init(). */
flight_context_t *flight_get_context(void);

/* BOOT_SETTLE before flight_init(). */
flight_state_t flight_get_state(void);

/* [USB-01..04] Whether a host is on the USB port, asked every loop before
 * dispatch_state(). Ignored from launch to landing: a flight is never
 * abandoned on the strength of it. */
void flight_set_usb_attached(flight_context_t *ctx, bool attached, uint32_t now);

/* [USB-08] With test mode on, a board on USB behaves as it does on battery.
 * Not changed from launch to landing. */
void flight_set_test_mode(flight_context_t *ctx, bool on, uint32_t now);

#endif
