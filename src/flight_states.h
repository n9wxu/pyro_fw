#ifndef FLIGHT_STATES_H
#define FLIGHT_STATES_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal.h"
#include "ground_test.h" /* ground_test_ctx_t embedded in flight_context_t */

/* What the power-up self-test found. Several can be true at once. */
#define DIAG_SENSOR_FAIL (1u << 0)
#define DIAG_FS_FAIL (1u << 1)
#define DIAG_CFG_RANGE (1u << 2) /* a pyro altitude setting is beyond the sensor */
#define DIAG_P1_OPEN (1u << 3)
#define DIAG_P1_SHORT (1u << 4)
#define DIAG_P2_OPEN (1u << 5)
#define DIAG_P2_SHORT (1u << 6)
#define DIAG_BROWNOUT (1u << 7) /* came back mid-flight after a power event */

/* The pyro ones can be fixed standing at the rocket; everything else means
 * safe it and walk away. This split is what picks the beep. */
#define DIAG_PYRO_ANY (DIAG_P1_OPEN | DIAG_P1_SHORT | DIAG_P2_OPEN | DIAG_P2_SHORT)
#define DIAG_FATAL_ANY (DIAG_SENSOR_FAIL | DIAG_FS_FAIL | DIAG_CFG_RANGE)
/* What the pad check re-derives every second, as distinct from what the
 * power-up test found once. */
#define DIAG_PAD_ANY (DIAG_PYRO_ANY | DIAG_CFG_RANGE)

/* The name a DIAG_* bit goes by on /api/status and the console. */
const char *flight_diag_name(uint16_t bit);

// System states
typedef enum {
    BOOT_SETTLE = 0, // wait for sensors to stabilize
    BOOT_CONTINUITY, // check pyro circuits
    BOOT_CALIBRATE,  // establish ground reference
    PAD_IDLE,
    ASCENT,
    FALLING,        /* free-fall before drogue fires */
    DROGUE_DESCENT, /* drogue deployed, before main chute fires */
    CHUTE_DESCENT,  /* main chute deployed, descending to landing */
    LANDED,
    /* Appended, not inserted. State numbers reach the flight log, the CSV,
     * the telemetry sentences and /api/status, so renumbering PAD_IDLE would
     * make every previously recorded flight read wrong. */
    BOOT_SENSOR, /* the pressure sensor is tested BEFORE the pyros */
    FAULT,       /* terminal: the board cannot fly and says so */
    STATE_COUNT
} flight_state_t;

// State machine events (drive transitions)
typedef enum {
    SEVT_NONE = 0,
    SEVT_DONE,
    SEVT_TIMER,
    SEVT_CAL_DONE,
    SEVT_LAUNCH,
    SEVT_ARMED,
    SEVT_APOGEE,
    /* The descent events name what the rocket is DOING, not which pyro was
     * commanded. A pyro that fired into a shredded canopy must not advance
     * the machine, and a canopy that opens late must not leave it behind. */
    SEVT_DROGUE,   /* descent has steadied at a drogue-like rate */
    SEVT_CHUTE,    /* descent has steadied at a main-like rate */
    SEVT_FREEFALL, /* a canopy that was working has stopped working */
    SEVT_LANDING,
    SEVT_FAULT, /* a power-up test failed; nothing recovers from this */
    /* A power event happened in flight and the board came back. The marker
     * written on the pad says where the ground was; the barometer says which
     * way the rocket is going. See brownout.h. */
    SEVT_RECOVER_ASCENT,
    SEVT_RECOVER_DESCENT,
} state_event_t;

// Forward declare for function pointer types
struct flight_context_t;

// Transition table entry
typedef state_event_t (*detect_fn)(struct flight_context_t *ctx, uint32_t now);
typedef void (*action_fn)(struct flight_context_t *ctx, uint32_t now);

typedef struct {
    flight_state_t from;
    state_event_t event;
    flight_state_t to;
    action_fn action;
} transition_t;

/* Pyro firing modes — defined in config.h */

#include "flight_events.h" /* EVT_* codes, stored in flight_sample_t.event */

// Flight sample (16 bytes)
typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa; /* or event data1 */
    int32_t altitude_cm; /* or event data2 */
    uint8_t state;
    uint8_t under_thrust;
    uint8_t event;      /* EVT_NONE = normal sample */
    uint8_t event_data; /* extra byte for event info */
} flight_sample_t;

/* config_t is defined in config.h */

// Flight context
/* The last 64 samples, 1 KB, for flight_save_csv(). The flight record is
 * hal_log's flight_log.csv; nothing on the flight path reads this back. */
#define FLIGHT_BUF_SIZE 64

typedef struct flight_context_t {
    flight_sample_t flight_buffer[FLIGHT_BUF_SIZE];
    uint16_t buf_head;
    uint16_t buf_tail;
    uint16_t buf_count;
    int32_t ground_pressure;
    int32_t max_altitude;
    int32_t last_altitude;
    int32_t vertical_speed_cms;
    int32_t prev_vertical_speed_cms;
    uint32_t launch_time;
    uint32_t apogee_time;
    uint32_t last_sample;
    uint32_t last_telemetry;
    uint32_t landing_stable_since;
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
    /* The board took the fire call and energised nothing. The channel is not
     * asked again: a refusal is a property of the board, not of the moment. */
    bool pyro1_refused;
    bool pyro2_refused;
    bool pyro1_fault; /* overcurrent detected during fire */
    bool pyro2_fault;
    bool pyro1_verify_fail; /* post-fire continuity still good (pyro didn't open) */
    bool pyro2_verify_fail;
    config_t config;
    flight_state_t current_state;
    int32_t filtered_pressure;
    // Boot state fields
    uint32_t boot_timer;
    // PAD_IDLE state
    uint32_t last_cont_check;
    bool buzzer_started;
    /* [FLT-LAUNCH-03] The first sample above 50 cm since the last one at or
     * below it: T+0, once the detector trips a hundred feet later. */
    bool pad_rising;
    uint32_t pad_rise_ms;
    // Safety features [DD-016, DD-017]
    int32_t max_speed_cms; // peak speed during ASCENT (for arming gate)
    /* The only record that arming preceded apogee [DD-022]. */
    uint32_t armed_time;
    uint32_t descent_start_time; // when DESCENT started (for landing timeout)
    uint32_t landing_time;       /* flight time stops here [WEB-UI-04] */
    uint32_t last_logged_ms;     /* log_rate_hz decimation, flight time */
    int32_t pad_speed_cms;       // vertical speed during PAD_IDLE (for launch confirm)
    // Last continuity status beep code [GND-TEST-01]
    uint8_t last_reason; /* beep_reason_t last said; for BEEP STATUS replay */

    /* Power-up self-test results. sensor_type is what hal_pressure_init()
     * returned; 0 means no sensor answered. A board that cannot measure
     * altitude cannot fly, so it must not report itself ready. */
    uint8_t sensor_type;
    bool fs_ok;

    /* ── Brownout recovery [FLT-BROWN-01..03] ─────────────────────
     * What the reset registers and the pad marker said at boot, kept so
     * /api/status and the flight log can report it. A recovered flight is
     * not the same flight -- the log restarts at the moment of recovery --
     * and nothing downstream should have to guess that from the data. */
    uint8_t reset_cause;
    uint8_t recovery;
    uint8_t recovery_why; /* cold_reason_t, when recovery is RECOVER_COLD */
    bool marker_written;
    bool marker_spent; /* invalidated at LANDED [FLT-BROWN-04] */

    /* [USB-01..04] A PC is on the USB port: the board is on a bench, so it
     * detects no launch, says no status code and writes no pad marker --
     * unless the operator has put it in test mode [USB-08], which is held
     * here in RAM and so is off at every boot. */
    bool usb_attached;
    bool test_mode;

    /* ── What is wrong, as distinct from what to do ──────────────
     *
     * The buzzer says one of three things, because three is the number of
     * actions available at the pad. The diagnosis is finer than that and is
     * still worth having, so it lives here as a bitmask and goes out on
     * /api/status -- which is read on a screen, where detail helps and
     * nobody has to count beeps in the wind. */
    uint16_t diag; /* DIAG_* bits */

    /* ── Mach gate [FLT-MACH-01] ──────────────────────────────────
     * The barometric sensor cannot be believed through the transonic
     * region, so apogee is not declared until the rocket has been slow
     * for a while. Only a fast ASCENT arms this; a flight that never
     * gets there is never gated, which is most of them. */
    bool mach_exceeded;
    uint32_t subsonic_since;

    /* ── Descent phase [FLT-DESC-01] ──────────────────────────────
     * The phase is read from the descent rate holding steady, because a
     * steady rate is what a working canopy looks like and an accelerating
     * one is what a failed canopy looks like. desc_ref_cms is the rate
     * when the current dwell window opened. */
    uint8_t desc_band;
    uint32_t desc_band_since;
    int32_t desc_ref_cms;
    uint32_t desc_fail_since;

    /* ── Emergency deploy [FLT-EMRG-01] ───────────────────────────
     * The retry budget is what makes the ladder terminate. The failure
     * window is the evidence the main is brought forward on: faster than a
     * drogue explains, not being slowed, since emrg_fail_since. */
    uint8_t pyro1_refires;
    bool main_forced; /* the ladder overrode pyro2's configured trigger */
    uint32_t emrg_fail_since;
    int32_t emrg_fail_ref_cms;
    // Ground test state machine [GND-TEST-01..04, DD-011]
    ground_test_ctx_t gt;
} flight_context_t;

// Flight init and dispatch
void flight_init(flight_context_t *ctx);
flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now);
void flight_update_outputs(flight_context_t *ctx, uint32_t now);
/* The flash writes the flight software owes. Call only where a write can
 * land: inside the hardware's flash window, and every tick on the host. */
void flight_flash_service(flight_context_t *ctx, uint32_t now);
/* Legacy compat — redirects to config module */
#define parse_config_ini(buf, cfg) config_parse_ini(buf, cfg)
/* [DAT-06] The ring buffer's last 64 samples as flight.csv. The simulator's
 * export; the flight record itself is hal_log's flight_log.csv. */
int flight_save_csv(flight_context_t *ctx);

/* Milliseconds since launch while airborne, frozen at the landing, and 0 on
 * the pad or in any state with no flight behind it [WEB-UI-04]. */
uint32_t flight_elapsed_ms(const flight_context_t *ctx, uint32_t now);

/* What brownout recovery made of this boot, for /api/status. */
const char *flight_recovery_text(const flight_context_t *ctx);

/* [PYR-DEPLOY-02] Energise one channel, if the shared element is free.
 *
 * The single place a channel is fired from, flight or ground test, because
 * the interlock has to sit below every caller. hal_pyro_is_firing() read
 * straight after hal_pyro_fire() is the board's acknowledgement: a board or
 * a mocked channel that energised nothing answers REFUSED. */
typedef enum { PYRO_BUSY, PYRO_REFUSED, PYRO_ENERGISED } pyro_fire_result_t;
pyro_fire_result_t flight_pyro_energise(uint8_t channel);

// Telemetry
void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state);

// Helpers used by state functions
void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st);

/* ── Config reload (runtime config update) ────────────────────────── */

/* Reload configuration from persistent storage into the flight context.
 * Only allowed in PAD_IDLE state for safety.
 * Returns:
 *   0 on success (config reloaded and applied)
 *  -1 if not in PAD_IDLE state (rejected for safety)
 *  -2 if config load failed (file missing or corrupted)
 *  -3 if config validation failed (invalid field values)
 */
int flight_config_reload(flight_context_t *ctx);

/* Get the global flight context pointer (for HTTP server access).
 * Returns NULL if flight_init() hasn't been called yet. */
flight_context_t *flight_get_context(void);

/* Get the current flight state (for HTTP server safety checks).
 * Returns BOOT_SETTLE if flight_init() hasn't been called yet. */
flight_state_t flight_get_state(void);

/* True from launch to landing. Anything that could disturb the flight -- a
 * reboot, a flash write, a firmware image -- is refused while this holds. */
bool flight_in_progress(void);

/* [USB-01..04] Whether a host is on the USB port, asked every loop before
 * dispatch_state(). Ignored from launch to landing: a flight is never
 * abandoned on the strength of it. */
void flight_set_usb_attached(flight_context_t *ctx, bool attached, uint32_t now);

/* [USB-08] With test mode on, a board on USB behaves as it does on battery.
 * Not changed from launch to landing. */
void flight_set_test_mode(flight_context_t *ctx, bool on, uint32_t now);

#endif
