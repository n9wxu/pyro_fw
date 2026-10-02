/*
 * The HTTP server: files (vfs.h), the API, uploads and OTA.
 *
 * Runs in the net task [DD-073]: every route, unit and file call here is on
 * core1 at P, beside Lua and storage, never on the flight task. What changes
 * flight state goes through flight_call() (rtos_tasks.h).
 *
 * Two halves. The routes below answer one request on an http_conn_t, which
 * parses from an rx ring and writes to a tx ring (http_conn.h) and never sees
 * a segment. The lwIP adapter at the bottom moves bytes between lwIP and
 * those rings: it queues what arrives, hands it over as the parser consumes
 * it -- which is also what reopens the TCP window -- and feeds tx to lwIP as
 * its send buffer allows.
 *
 * [WEB-HTTP-05, WEB-HTTP-06] lwIP callbacks only queue.
 * http_server_transport() moves bytes and nothing else; every other step is a
 * work unit (http_work.h), run by http_server_work().
 */
#include "lwip/tcp.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "lwip/priv/tcp_priv.h"
#include "board_id.h"
#include "board_if.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pico/stdlib.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <pico_fota_bootloader/core.h>
#include "device_status.h"
#include "version.h"
#include "flight_states.h"
#include "pressure_processing.h"

#if PYRO_HAS_LUA
#include "lua_app.h"
#include "lua_core1.h"
#endif

#include "flash_op.h"
#include "hal_storage.h"
#include "vfs.h"
#if PYRO_HAS_SD
#include "sd_card.h"
#include "hr_log.h"
#include "ff.h"
#endif
#include "rtos_tasks.h"
#include "buzzer.h"
#include "pin_store.h"
#include "brownout.h"
#include "beep_store.h"
#include "pin_caps.h"
#include "buzzer.h"
#include "pyro_release.h"
#include "http_conn.h"
#include "http_routes.h"
#include "http_server.h"
#include "http_work.h"
#include "ota_bounds.h"
#include "lfs_mount.h"
#include "status_json.h"
#include "flight_log.h"
#include "pressure_trace.h"
#include "net_stats.h"
#if PYRO_HAS_BENCH_FLIGHT
#include "bench_flight.h"
#endif

extern uint32_t hal_time_ms(void);

extern void hal_telemetry_send(const char *sentence);
#define DBG(fmt, ...)                                                                                                  \
    do {                                                                                                               \
        char _b[128];                                                                                                  \
        snprintf(_b, sizeof(_b), "HTTP: " fmt "\r\n", ##__VA_ARGS__);                                                  \
        hal_telemetry_send(_b);                                                                                        \
    } while (0)

extern const char *pressure_sensor_name(void);
/* hal_common.c. pres_rejects counts readings refused as impossible: on a
 * healthy board it stays at zero. */
extern uint32_t hal_pressure_interval_min_us(void);
extern uint32_t hal_pressure_interval_max_us(void);
extern uint32_t hal_pressure_stamp_lag_max_us(void);
extern uint32_t hal_pressure_waits(void);
extern uint32_t hal_pressure_rate_hz(void);
extern uint32_t hal_pressure_rejects(void);
extern uint32_t hal_pressure_flashed(void);

/* [WEB-API-07] No Access-Control-Allow-Origin. Every page that talks to the
 * board is served by the board, so every legitimate request is same-origin;
 * "*" let any site read the API. Cross-Origin-Resource-Policy keeps another
 * site from embedding a response. */
#define COMMON_HDRS "Cross-Origin-Resource-Policy: same-origin\r\n"

#define JSON "application/json"
#define TEXT "text/plain"

/* ── Connections ──────────────────────────────────────────────────────
 *
 * A link is one lwIP pcb; a conn is one HTTP exchange, with its rings. A pcb
 * gets a conn when it first sends something, so the idle sockets a browser
 * opens speculatively cost a link and nothing more. While every conn is busy,
 * what a link receives waits in its pbuf queue, unacknowledged to the
 * application, so TCP flow control holds the sender back. */

#define CONN_POOL_SIZE 4
#define LINK_POOL_SIZE MEMP_NUM_TCP_PCB
/* No byte moved either way for this long: the peer is gone. */
#define HTTP_IDLE_MS 20000u

struct conn;

typedef struct {
    struct tcp_pcb *pcb;  /* NULL: free */
    struct pbuf *pending; /* received, not yet in the rx ring */
    bool fin;             /* the peer has closed its side */
    bool counted_wait;
    uint32_t seq; /* accept order: waiting links are served in turn */
    uint32_t last_ms;
    struct conn *conn;
} link_t;

typedef struct conn {
    http_conn_t h;
    link_t *link; /* NULL: free */
    route_t route;
    vfs_file_t file;
    bool file_open;
    bool file_writing; /* an upload into dest's ".part", renamed when whole */
    bool fs_held;      /* counted by hal_fs_enter() until conn_release() */
    bool reboot_when_sent;
    char dest[HTTP_PATH_MAX];
    union {
        status_snap_t status;
        struct {
            flog_csv_t csv;
            uint32_t len;
        } flog;
    };
} conn_t;

#define CONN_OF(hc) ((conn_t *)((char *)(hc) - offsetof(conn_t, h)))

_Static_assert(CONN_POOL_SIZE == HTTP_WORK_SLOTS, "a work slot is a connection");
_Static_assert(STATUS_JSON_MAX <= HTTP_WORK_SIZE, "the status is rendered into the work buffer");
_Static_assert(LFS_FILE_BUF_SIZE <= HTTP_WORK_SIZE, "the work buffer is a streamed file's cache");

static link_t links[LINK_POOL_SIZE];
static conn_t conns[CONN_POOL_SIZE];
static uint32_t link_seq;
static conn_t *ota_conn; /* the OTA state below is one image at a time */

/* ── OTA firmware update state ────────────────────────────────────── */

/* pico_fota_bootloader's linker symbols: their addresses are the values. The
 * download slot is the swap space; past it is littlefs. */
extern uint32_t __FLASH_DOWNLOAD_SLOT_START;
extern uint32_t __FLASH_SWAP_SPACE_LENGTH;
#define OTA_SLOT_OFF ((uint32_t) & __FLASH_DOWNLOAD_SLOT_START - XIP_BASE)
#define OTA_SLOT_BYTES ((uint32_t) & __FLASH_SWAP_SPACE_LENGTH)

static uint8_t ota_buf[FLASH_SECTOR_SIZE] __attribute__((aligned(FLASH_PAGE_SIZE)));
static uint32_t ota_offset; /* bytes written so far */
static uint16_t ota_buf_fill;
static bool ota_failed;
static bool pfb_started; /* pfb_begin_op() has run for this image */

/* One sector: an erase and a program, each its own lockout, so the system
 * stops for one operation at a time. */
static bool ota_flush(void) {
    if (ota_buf_fill == 0)
        return true;
    if (!ota_sector_fits(ota_offset, FLASH_SECTOR_SIZE, OTA_SLOT_BYTES))
        return false;
    while (ota_buf_fill & (FLASH_PAGE_SIZE - 1))
        ota_buf[ota_buf_fill++] = 0xFF;
    uint32_t addr = OTA_SLOT_OFF + ota_offset;
    if (flash_op_erase(addr, FLASH_SECTOR_SIZE) != 0 || flash_op_program(addr, ota_buf, ota_buf_fill) != 0)
        return false;
    ota_offset += FLASH_SECTOR_SIZE;
    ota_buf_fill = 0;
    return true;
}

/* pico_fota_bootloader's marks mask interrupts and write flash themselves;
 * under the lockout, so the other core is out of XIP meanwhile. */

/* An image starts with the slot marked invalid: a valid mark left from an
 * earlier image would otherwise swap in this one half-written, should the
 * board reset mid-transfer. The whole-slot erase pfb_initialize_download_slot()
 * does would stop both cores for seconds; ota_flush() erases each sector
 * before it writes it instead. */
static void pfb_begin_op(void *arg) {
    (void)arg;
    pfb_firmware_commit();
    pfb_mark_download_slot_as_invalid();
}

static void pfb_valid_op(void *arg) {
    (void)arg;
    pfb_mark_download_slot_as_valid();
}

/* Short when a sector would not write. */
static uint16_t ota_write(const void *data, uint16_t len) {
    const uint8_t *src = (const uint8_t *)data;
    uint16_t done = 0;
    while (len > 0) {
        uint16_t space = FLASH_SECTOR_SIZE - ota_buf_fill;
        uint16_t chunk = (len < space) ? len : space;
        memcpy(ota_buf + ota_buf_fill, src, chunk);
        ota_buf_fill += chunk;
        src += chunk;
        len -= chunk;
        done += chunk;
        if (ota_buf_fill == FLASH_SECTOR_SIZE && !ota_flush())
            break;
    }
    return done;
}

/* ── Content type ─────────────────────────────────────────────────── */

static const char *content_type_hdr(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    if (strcmp(ext, ".js") == 0)
        return "application/javascript";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".json") == 0)
        return "application/json";
    if (strcmp(ext, ".csv") == 0)
        return "text/csv";
    return "application/octet-stream";
}

/* [WEB-API-08, WEB-API-10, DD-058] The API is live in flight; the filesystem
 * is not. The flight log holds it from launch until its tail is flushed, and
 * a request that needs it is answered 423 before any mount. The hold is
 * released in conn_release(). */
static void respond_fs_locked(http_conn_t *hc) {
    http_respond_str(hc, 423, JSON, "{\"error\":\"the flight log holds the filesystem\"}");
}

static bool fs_take(conn_t *c) {
    if (c->fs_held) {
        return true;
    }
    if (hal_fs_enter() != 0) {
        respond_fs_locked(&c->h);
        return false;
    }
    c->fs_held = true;
    return true;
}

#define OLD_LOG_PATH "flight_log.csv" /* before DD-062: served as it is */
#define CSV_DISPOSITION "Content-Disposition: attachment; filename=\"flight.csv\"\r\n"
#define EMPTY_LOG_CSV "time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event\r\n"

/* ── API handlers ─────────────────────────────────────────────────── */

/* Indexed by flight_state_t, in its order: BOOT_SENSOR and FAULT come late
 * because the enum's numbers reach the flight log. */
static const char *state_names[] = {"BOOT_SETTLE", "BOOT_CONTINUITY", "BOOT_CALIBRATE", "PAD_IDLE",
                                    "ASCENT",      "FALLING",         "DROGUE_DESCENT", "CHUTE_DESCENT",
                                    "LANDED",      "BOOT_SENSOR",     "FAULT",          "GROUND_TEST"};
#define STATE_NAME_COUNT ((int)(sizeof(state_names) / sizeof(state_names[0])))

/* The flight task's pacing (main_hardware.c), reported so the budget a board
 * declares in board.cmake is measured rather than trusted. */
extern volatile uint32_t loop_count, loop_max_us, loop_overruns, loop_late_max_us;
extern volatile uint32_t stage_max_us[];
extern volatile uint32_t stage1_part_max_us[]; /* hal_common.c: TinyUSB, lwIP, HTTP transport, mDNS */
extern volatile bool boot_prev_watchdog;       /* main_hardware.c */
extern volatile int32_t boot_prev_stage;
extern volatile uint32_t boot_prev_stage_ms;

#include "board_identity.h"
#include "board_selftest.h"

/* POST /api/beeps/play: one sound, once, so an operator hears it before
 * choosing it [BUZ-CODE-04, USB-03]. PAD_IDLE only: the buzzer is the flight
 * software's voice and a browser must not talk over a launch. */
/* How long a request waits for the flight task to take its change. */
#define CALL_MS 200u

static void beep_play_call(void *arg) {
    buzzer_play_spec((const beep_spec_t *)arg, 0, 1);
}

static void apply_api_beep_play(http_conn_t *hc, const char *body) {
    char jb[192];
    int jn;
    uint16_t status;

    beep_spec_t sp = {BK_CODE, 0, 0};
    const char *pk = strstr(body, "\"kind\"");
    if (pk) {
        const char *q = strchr(pk + 6, '"');
        const char *q2 = q ? strchr(q + 1, '"') : NULL;
        if (q && q2) {
            for (int k = 0; k <= BK_CODE; k++) {
                const char *kn = beep_codes_kind_name((beep_kind_t)k);
                if ((size_t)(q2 - q - 1) == strlen(kn) && strncmp(q + 1, kn, strlen(kn)) == 0) {
                    sp.kind = (uint8_t)k;
                    break;
                }
            }
        }
    }
    const char *p1 = strstr(body, "\"d1\"");
    const char *p2 = strstr(body, "\"d2\"");
    if (p1 && strchr(p1, ':')) {
        sp.d1 = (uint8_t)atoi(strchr(p1, ':') + 1);
    }
    if (p2 && strchr(p2, ':')) {
        sp.d2 = (uint8_t)atoi(strchr(p2, ':') + 1);
    }

    bool code_ok =
        sp.kind != BK_CODE || (sp.d1 >= BEEP_DIGIT_MIN && sp.d1 <= BEEP_DIGIT_MAX && sp.d2 <= BEEP_DIGIT_MAX);

    if (flight_get_state() != PAD_IDLE) {
        jn = snprintf(jb, sizeof(jb), "{\"error\":\"Device not ready (state=%s)\"}",
                      state_names[flight_get_state() < STATE_NAME_COUNT ? flight_get_state() : 0]);
        status = 409;
    } else if (!code_ok) {
        jn = snprintf(jb, sizeof(jb), "{\"error\":\"each beep count must be %d to %d\"}", BEEP_DIGIT_MIN,
                      BEEP_DIGIT_MAX);
        status = 400;
    } else if (!pin_store_has_buzzer()) {
        /* MK1A fits none: "playing" would be a lie told in silence. */
        jn = snprintf(jb, sizeof(jb), "{\"error\":\"this board has no buzzer fitted\"}");
        status = 409;
    } else if (!flight_call(beep_play_call, &sp, CALL_MS)) {
        jn = snprintf(jb, sizeof(jb), "{\"error\":\"the flight task did not take the sound\"}");
        status = 503;
    } else {
        /* Once, with no gap: an audition is a sample, not a state. */
        jn = snprintf(jb, sizeof(jb), "{\"status\":\"playing\",\"kind\":\"%s\",\"d1\":%u,\"d2\":%u}",
                      beep_codes_kind_name((beep_kind_t)sp.kind), (unsigned)sp.d1, (unsigned)sp.d2);
        status = 200;
    }

    http_respond(hc, status, JSON, jb, (uint32_t)jn);
}

/* POST /api/test_mode/on, /api/test_mode/off [USB-08]. Held in RAM, so a
 * reboot ends it. In flight the flight layer ignores it, and the answer says
 * the mode it kept. */

static void test_mode_call(void *arg) {
    flight_set_test_mode(flight_get_context(), *(const bool *)arg, hal_time_ms());
}

static void apply_api_test_mode(http_conn_t *hc, bool on) {
    flight_context_t *ctx = flight_get_context();
    if (!ctx) {
        http_respond_str(hc, 503, JSON, "{\"error\":\"not ready\"}");
        return;
    }
    if (!flight_call(test_mode_call, &on, CALL_MS)) {
        http_respond_str(hc, 503, JSON, "{\"error\":\"the flight task did not take the change\"}");
        return;
    }
    http_respond_str(hc, 200, JSON, ctx->test_mode ? "{\"test_mode\":true}" : "{\"test_mode\":false}");
}

#if PYRO_HAS_BENCH_FLIGHT
/* POST /api/sim/flight?apogee=&boost=&drogue=&main_alt=&main=&thin=&pad=,
 * POST /api/sim/stop, GET /api/sim [SIM-01, DD-078]. Metres, seconds and
 * metres a second; thin=0 holds the drogue's rate at every height. */
typedef struct {
    fsim_params_t p;
    bf_start_t rc;
} sim_start_t;

static void sim_start_call(void *arg) {
    sim_start_t *a = arg;
    const flight_context_t *ctx = flight_get_context();
    a->rc = bench_flight_start(&a->p, (float)pp_ground_pressure(), flight_get_state() == PAD_IDLE,
                               ctx && ctx->test_mode, time_us_64());
}

static void sim_stop_call(void *arg) {
    (void)arg;
    bench_flight_stop();
}

static float query_f(const char *path, const char *key, float dflt) {
    const char *q = strstr(path, key);
    return q ? strtof(q + strlen(key), NULL) : dflt;
}

static void apply_sim_flight(http_conn_t *hc, const char *path) {
    sim_start_t a = {.p = {.apogee_m = query_f(path, "apogee=", 3000.0f),
                           .boost_s = query_f(path, "boost=", 2.0f),
                           .drogue_ms = query_f(path, "drogue=", 25.0f),
                           .main_alt_m = query_f(path, "main_alt=", 300.0f),
                           .main_ms = query_f(path, "main=", 6.0f),
                           .thin_air = query_f(path, "thin=", 1.0f) != 0.0f,
                           .pad_s = query_f(path, "pad=", 5.0f)},
                     .rc = BF_BAD_PROFILE};
    if (!flight_call(sim_start_call, &a, CALL_MS)) {
        http_respond_str(hc, 503, JSON, "{\"error\":\"the flight task did not take the start\"}");
        return;
    }
    static const char *const why[] = {"started", "not on the pad", "test mode is off", "one is flying",
                                      "the profile cannot fly"};
    char jb[80];
    int jn = snprintf(jb, sizeof(jb), a.rc == BF_STARTED ? "{\"status\":\"%s\"}" : "{\"error\":\"%s\"}", why[a.rc]);
    http_respond(hc, a.rc == BF_STARTED ? 200 : a.rc == BF_BAD_PROFILE ? 400 : 409, JSON, jb, (uint32_t)jn);
}

static void serve_api_sim(http_conn_t *hc) {
    bench_flight_status_t s;
    bench_flight_status(&s);
    bool suspect, stuck;
    pfit_t f = pp_last_fit(&suspect, &stuck);
    const flight_context_t *ctx = flight_get_context();
    int n = snprintf((char *)hc->work, sizeof(hc->work),
                     "{\"flying\":%s,\"mocked\":%s,\"flights\":%lu,\"phase\":\"%s\",\"t_s\":%.2f,\"t_apogee_s\":%.2f,"
                     "\"alt_m\":%.1f,\"peak_m\":%.1f,\"pa\":%.1f,\"ground_pa\":%.1f,\"fires\":[%lu,%lu],"
                     "\"apogee_m\":%.0f,\"boost_s\":%.2f,\"drogue_ms\":%.1f,\"main_alt_m\":%.0f,"
                     "\"main_ms\":%.1f,\"thin_air\":%s,\"pad_s\":%.1f,"
                     "\"fit\":{\"pa\":%.2f,\"pdot\":%.3f,\"pddot\":%.4f,\"rms\":%.3f,\"worst\":%.3f,\"n\":%u,"
                     "\"valid\":%s,\"suspect\":%s,\"stuck\":%s,\"sigma\":%.3f},"
                     "\"mach_lock\":%s,\"release_since\":%lu,\"fit_clean\":%s}",
                     s.flying ? "true" : "false", s.mocked ? "true" : "false", (unsigned long)s.flights,
                     fsim_phase_name(s.phase), (double)s.t_s, (double)s.t_apogee_s, (double)s.alt_m, (double)s.peak_m,
                     (double)s.pa, (double)s.ground_pa, (unsigned long)s.fires[0], (unsigned long)s.fires[1],
                     (double)s.p.apogee_m, (double)s.p.boost_s, (double)s.p.drogue_ms, (double)s.p.main_alt_m,
                     (double)s.p.main_ms, s.p.thin_air ? "true" : "false", (double)s.p.pad_s, (double)f.p,
                     (double)f.pdot, (double)f.pddot, (double)f.rms, (double)f.worst, (unsigned)f.n,
                     f.valid ? "true" : "false", suspect ? "true" : "false", stuck ? "true" : "false",
                     (double)pp_sigma_pa(), ctx && ctx->mach_lock ? "true" : "false",
                     (unsigned long)(ctx ? ctx->release_since : 0), ctx && ctx->fit_clean ? "true" : "false");
    http_respond(hc, 200, JSON, hc->work, (uint32_t)n);
}
#endif

/* GET /api/beeps [BUZ-CODE-04, BUZ-CODE-09]: the outcomes, their meanings
 * and the personalities. The vocabulary travels with the data, as in
 * /api/pins/caps, so app.js holds no copy of it. */
static void serve_api_beeps(http_conn_t *hc) {
    char *buf = (char *)hc->work;
    const size_t cap = sizeof(hc->work);

    const beep_table_t *t = beep_store_current();
    char reason_esc[128];
    const char *br = beep_store_reason();
    json_escape(reason_esc, (int)sizeof(reason_esc), br, (int)strlen(br));

    int pos = snprintf(buf, cap,
                       "{\"digit_min\":%d,\"digit_max\":%d,\"has_buzzer\":%s,\"active\":%u,\"reason\":\"%s\","
                       "\"kinds\":[\"silent\",\"chirp\",\"tone\",\"code\"],\"outcomes\":[",
                       BEEP_DIGIT_MIN, BEEP_DIGIT_MAX, pin_store_has_buzzer() ? "true" : "false", (unsigned)t->active,
                       reason_esc);

    for (int i = 0; i < BEEP_REASON_COUNT && pos > 0 && pos < (int)cap; i++) {
        char desc[192];
        const char *d = beep_codes_description((beep_reason_t)i);
        json_escape(desc, (int)sizeof(desc), d, (int)strlen(d));
        pos += snprintf(buf + pos, cap - (size_t)pos, "%s{\"key\":\"%s\",\"what\":\"%s\"}", i ? "," : "",
                        beep_codes_key((beep_reason_t)i), desc);
    }

    if (pos > 0 && pos < (int)cap) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "],\"personalities\":[");
    }
    for (int i = 0; i < BEEP_PERSONALITY_COUNT && pos > 0 && pos < (int)cap; i++) {
        const beep_personality_t *p = &t->p[i];
        char nm[BEEP_NAME_MAX * 2 + 2];
        json_escape(nm, (int)sizeof(nm), p->name, (int)strlen(p->name));
        pos += snprintf(buf + pos, cap - (size_t)pos,
                        "%s{\"name\":\"%s\",\"gap\":%u,\"repeat\":%u,\"split\":%s,\"spec\":{", i ? "," : "", nm,
                        (unsigned)p->gap_ms, (unsigned)p->repeat, p->split_pyro ? "true" : "false");
        for (int r = 0; r < BEEP_REASON_COUNT && pos > 0 && pos < (int)cap; r++) {
            pos +=
                snprintf(buf + pos, cap - (size_t)pos, "%s\"%s\":{\"kind\":\"%s\",\"d1\":%u,\"d2\":%u}", r ? "," : "",
                         beep_codes_key((beep_reason_t)r), beep_codes_kind_name((beep_kind_t)p->spec[r].kind),
                         (unsigned)p->spec[r].d1, (unsigned)p->spec[r].d2);
        }
        if (pos > 0 && pos < (int)cap) {
            pos += snprintf(buf + pos, cap - (size_t)pos, "}}");
        }
    }
    if (pos > 0 && pos < (int)cap) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "]}");
    }

    if (pos < 0 || pos >= (int)cap) {
        http_respond_str(hc, 500, JSON, "{\"error\":\"beep table exceeds the response buffer\"}");
        return;
    }
    http_respond(hc, 200, JSON, buf, (uint32_t)pos);
}

/* Apply a complete beep.ini body and answer. */
static void apply_api_beeps(http_conn_t *hc, char *body) {
    static beep_table_t merged;
    merged = *beep_store_current();
    beep_codes_parse_ini(body, &merged);

    beep_verdict_t v = beep_store_save(&merged);
    char jb[224];
    int jn;
    uint16_t status;
    if (v.err == BEEP_OK) {
        jn = snprintf(jb, sizeof(jb), "{\"status\":\"ok\",\"reboot_required\":false}");
        status = 200;
    } else {
        char esc[160];
        json_escape(esc, (int)sizeof(esc), v.what, (int)strlen(v.what));
        /* Which personality, as well as which outcome: with three slots, "two
         * outcomes sound the same" is not actionable without knowing where. */
        jn = snprintf(jb, sizeof(jb), "{\"error\":\"%s\",\"personality\":%d,\"reason\":\"%s\",\"code\":%d}", esc,
                      v.personality, v.reason >= 0 ? beep_codes_key((beep_reason_t)v.reason) : "", (int)v.err);
        status = 400;
    }
    http_respond(hc, status, JSON, jb, (uint32_t)jn);
}

typedef struct {
    config_t cfg;
    int rc;
} config_apply_t;

static void config_apply_call(void *arg) {
    config_apply_t *ap = (config_apply_t *)arg;
    ap->rc = flight_config_apply(flight_get_context(), &ap->cfg);
}

/* POST /api/config [WEB-API-03]. */
static void apply_api_config(http_conn_t *hc, char *cfgbuf) {
    flight_state_t state = flight_get_state();
    const flight_context_t *fctx = flight_get_context();

    if (!fctx) {
        http_respond_str(hc, 503, JSON, "{\"error\":\"not ready\"}");
        return;
    } else if (state != PAD_IDLE) {
        DBG("POST /api/config REJECT state=%u (need PAD_IDLE=3)", (unsigned)state);
        char err_msg[160];
        int n = snprintf(err_msg, sizeof(err_msg),
                         "{\"error\":\"Device not ready (state=%s)\","
                         "\"state\":\"%s\",\"reboot_required\":true}",
                         state_names[state < STATE_NAME_COUNT ? state : 0],
                         state_names[state < STATE_NAME_COUNT ? state : 0]);
        http_respond(hc, 409, JSON, err_msg, (uint32_t)n);
        return;
    } else {
        /* [CFG-06] Each tab posts only its own keys, so the body is parsed
         * over the running config and re-serialised, never written as it is;
         * [CFG-08] a key config_parse_ini() does not know is dropped. */
        config_t merged = fctx->config;
        config_parse_ini(cfgbuf, &merged);
        char cfgout[512];
        int cfgn = config_serialize_ini(&merged, cfgout, (int)sizeof(cfgout));
        if (cfgn <= 0) {
            /* More than hal_config_load() reads back. */
            http_respond_str(hc, 500, JSON, "{\"error\":\"Merged config exceeds the 512-byte budget\"}");
            return;
        }

        int wr = hal_fs_write_file("config.ini", cfgout, cfgn);
        DBG("POST /api/config write=%d", wr);
        if (wr != 0) {
            http_respond_str(hc, 500, TEXT, "config.ini write failed");
            return;
        }
        /* Read back as the next boot will, then applied by the flight task,
         * which owns the running config. */
        static config_apply_t ap;
        config_set_defaults(&ap.cfg);
        ap.rc = hal_config_load(&ap.cfg) < 0 ? -2 : 0;
        if (ap.rc == 0 && !flight_call(config_apply_call, &ap, CALL_MS))
            ap.rc = -4;
        if (ap.rc == 0) {
            DBG("POST /api/config OK (applied)");
            http_respond_str(hc, 200, JSON, "{\"status\":\"ok\",\"applied\":true}");
        } else {
            DBG("POST /api/config WARN reload_result=%d", ap.rc);
            http_respond_str(hc, 500, JSON, "{\"error\":\"Config saved but reload failed\",\"reboot_required\":true}");
        }
    }
}

/* POST /api/pins. */
static void apply_api_pins(http_conn_t *hc, char *body) {
    flight_state_t st = flight_get_state();

    if (st != PAD_IDLE) {
        /* Same interlock as /api/config: a pin map that changes under a flying
         * board would move the pyro pins mid-flight. */
        char jb[160];
        int jn = snprintf(jb, sizeof(jb), "{\"error\":\"Device not ready (state=%s)\",\"reboot_required\":true}",
                          state_names[st < STATE_NAME_COUNT ? st : 0]);
        http_respond(hc, 409, JSON, jb, (uint32_t)jn);
    } else {
        /* [CFG-06] Merged, as /api/config is: a partial post must not
         * release a channel by omitting its key. Static: about 300 bytes,
         * and the net task is the only caller. */
        static pin_assign_t merged;
        merged = *pin_store_current();
        pin_assign_parse_ini(body, &merged);

        pin_verdict_t v = pin_store_save(&merged);
        char jb[224];
        int jn;
        uint16_t status;
        if (v.err == PIN_OK) {
            jn = snprintf(jb, sizeof(jb), "{\"status\":\"ok\",\"reboot_required\":true}");
            status = 200;
        } else {
            char esc[160];
            json_escape(esc, (int)sizeof(esc), v.what, (int)strlen(v.what));
            jn =
                snprintf(jb, sizeof(jb), "{\"error\":\"%s\",\"pin\":%u,\"code\":%d}", esc, (unsigned)v.pin, (int)v.err);
            status = 400;
        }
        http_respond(hc, status, JSON, jb, (uint32_t)jn);
    }
}

/* GET /api/pins/caps: what this board offers, what is assigned now, and the
 * vocabulary to read both in. `fn` names the capability bits and `roles` the
 * bit each role needs, so the UI filters its menus by the rule
 * pin_assign_validate() enforces rather than by a copy of it. */
static void serve_api_pin_caps(http_conn_t *hc) {
    /* The largest response the server builds, and what sizes HTTP_WORK_SIZE:
     * 30 GPIO rows of at most ~110 bytes, plus ~1000 for the fn map, the
     * roles and the protection note. */
    char *buf = (char *)hc->work;
    const size_t cap = sizeof(hc->work);

    int n_caps = 0;
    const pin_cap_t *caps = pin_caps_table(&n_caps);
    const pin_assign_t *pa = pin_store_current();

    /* One row per bit, from pin_model.h. Listed rather than derived because a
     * bit's NAME is the one thing the macro cannot supply. */
    static const struct {
        const char *name;
        uint32_t bit;
    } fn_bits[] = {
        {"pyro_fire", FN_PYRO_FIRE},
        {"pyro_common", FN_PYRO_COMMON},
        {"pyro_sense", FN_PYRO_SENSE},
        {"buzzer", FN_BUZZER},
        {"uart_tx", FN_UART_TX},
        {"uart_rx", FN_UART_RX},
        {"i2c_sda", FN_I2C_SDA},
        {"i2c_scl", FN_I2C_SCL},
        {"led", FN_LED},
        {"digital", FN_DIGITAL},
        {"pwm", FN_PWM},
        {"serial", FN_SERIAL},
        {"pixel", FN_PIXEL},
        {"bridge", FN_BRIDGE},
        {"analog", FN_ANALOG},
    };
    static const char *group_names[] = {"none", "ch1", "ch2", "common"};

    char note[320];
    json_escape(note, (int)sizeof(note), pin_caps_protection_note(), (int)strlen(pin_caps_protection_note()));

    int pos = snprintf(
        buf, cap,
        "{\"board\":\"%s\",\"topology\":\"%s\",\"bridge_possible\":%s,"
        "\"protection_note\":\"%s\","
        "\"pyro1_released\":%s,\"pyro2_released\":%s,\"reserved_mask\":%u,"
        "\"buzzer_pin\":%d,\"buzzer_on\":%d,"
        "\"ground_test\":\"%s\",\"gt_pin\":%d,\"gt_drive_pin\":%d,\"fn\":{",
        PYRO_BOARD_NAME, pin_caps_topology_name(), pin_caps_bridge_possible() ? "true" : "false", note,
        pa->pyro1_released ? "true" : "false", pa->pyro2_released ? "true" : "false", (unsigned)FN_BOARD_RESERVED,
        /* buzzer_pin: the setting, -1 for the board's own;
         * buzzer_on: where it is. */
        pa->buzzer_pin == PIN_BUZZER_BOARD ? -1 : (int)pa->buzzer_pin,
        pin_assign_buzzer_pin(pa) == PIN_BUZZER_BOARD ? -1 : (int)pin_assign_buzzer_pin(pa),
        /* [GND-TEST-12] -1 where a pad is not assigned. */
        pa->gt_wiring == GT_WIRING_GROUND ? "ground" : (pa->gt_wiring == GT_WIRING_PAIR ? "pair" : "none"),
        pa->gt_pin == PIN_GT_UNSET ? -1 : (int)pa->gt_pin,
        pa->gt_drive_pin == PIN_GT_UNSET ? -1 : (int)pa->gt_drive_pin);

    for (unsigned i = 0; i < sizeof(fn_bits) / sizeof(fn_bits[0]) && pos > 0 && pos < (int)cap; i++) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "%s\"%s\":%u", i ? "," : "", fn_bits[i].name,
                        (unsigned)fn_bits[i].bit);
    }

    if (pos > 0 && pos < (int)cap) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "},\"roles\":[");
    }
    for (int i = 0; i < pin_assign_role_count() && pos > 0 && pos < (int)cap; i++) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "%s{\"r\":\"%s\",\"needs\":%u}", i ? "," : "",
                        pin_assign_role_name(i), (unsigned)pin_assign_role_needs(i));
    }

    if (pos > 0 && pos < (int)cap) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "],\"pins\":[");
    }
    for (int i = 0; i < n_caps && pos > 0 && pos < (int)cap; i++) {
        uint8_t pin = caps[i].pin;
        uint8_t role = (pin < PIN_ASSIGN_MAX_GPIO) ? pa->role[pin] : LUA_ROLE_OFF;
        const char *nm = (pin < PIN_ASSIGN_MAX_GPIO) ? pa->name[pin] : "";
        char nesc[LUA_NAME_MAX * 2 + 2];
        json_escape(nesc, (int)sizeof(nesc), nm, (int)strlen(nm));
        char lesc[64];
        json_escape(lesc, (int)sizeof(lesc), caps[i].label, (int)strlen(caps[i].label));
        pos += snprintf(buf + pos, cap - (size_t)pos,
                        "%s{\"p\":%u,\"f\":%u,\"g\":\"%s\",\"lbl\":\"%s\",\"role\":\"%s\",\"name\":\"%s\","
                        "\"held\":%s}",
                        i ? "," : "", (unsigned)pin, (unsigned)caps[i].functions, group_names[caps[i].group], lesc,
                        pin_assign_role_name_of(role), nesc, pin_assign_is_reserved(pa, pin) ? "true" : "false");
    }
    if (pos > 0 && pos < (int)cap) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "]}");
    }

    /* Truncated JSON parses as nothing: say so instead. */
    if (pos < 0 || pos >= (int)cap) {
        http_respond_str(hc, 500, JSON, "{\"error\":\"capability table exceeds the response buffer\"}");
        return;
    }
    http_respond(hc, 200, JSON, buf, (uint32_t)pos);
}

/* [WEB-API-11] What the flight task owns, taken by the flight task itself at
 * the head of its period (flight_call()), so the fields agree with one
 * another: a read from the net task could land between two of its writes. */
static void status_capture_flight(void *arg) {
    extern beep_reason_t beep_reason_for_diag(uint16_t diag);
    static const char *mode_names[] = {"none", "fallen", "agl", "speed", "delay"};
    status_snap_t *s = (status_snap_t *)arg;
    const flight_context_t *fctx = flight_get_context();

    s->state = g_status.state < STATE_NAME_COUNT ? state_names[g_status.state] : "UNKNOWN";
    s->alt_cm = g_status.altitude_cm;
    s->max_alt_cm = g_status.max_altitude_cm;
    s->vspeed_cms = g_status.vertical_speed_cms;
    s->pressure_pa = g_status.pressure_pa;
    s->pyro_cont[0] = g_status.pyro1_continuity;
    s->pyro_cont[1] = g_status.pyro2_continuity;
    s->pyro_adc[0] = g_status.pyro1_adc;
    s->pyro_adc[1] = g_status.pyro2_adc;
    s->pyro_fired[0] = g_status.pyro1_fired;
    s->pyro_fired[1] = g_status.pyro2_fired;
    s->armed = g_status.pyros_armed;
    s->flight_ms = g_status.flight_time_ms;
    s->pyro_mode[0] = g_status.pyro1_mode < 5 ? mode_names[g_status.pyro1_mode] : "?";
    s->pyro_mode[1] = g_status.pyro2_mode < 5 ? mode_names[g_status.pyro2_mode] : "?";
    s->pyro_value[0] = g_status.pyro1_value;
    s->pyro_value[1] = g_status.pyro2_value;
    s->units = g_status.units;
    s->log_rate = config_log_rate_name(fctx ? fctx->config.log_rate : LOG_RATE_1HZ);
    memcpy(s->rocket_id, (const char *)g_status.rocket_id, sizeof(s->rocket_id) - 1);
    memcpy(s->rocket_name, (const char *)g_status.rocket_name, sizeof(s->rocket_name) - 1);
    s->sensor = pressure_sensor_name();

    /* Raw counts rather than volts, so a marginal reading stays visible. */
    board_pyro_raw_t praw = {0};
    bool raw = board_pyro_raw(&praw);
    s->pyro_bus_q = raw ? (int32_t)praw.bus_quiescent : -1;
    s->pyro_bus_adc = raw ? (int32_t)praw.bus_biased : -1;
    s->pyro_vbat_adc = raw ? (int32_t)praw.vbat : -1;

    s->loop_max_us = loop_max_us;
    s->loop_overruns = loop_overruns;
    s->loop_late_max_us = loop_late_max_us;
    s->loop_count = loop_count;
    for (int i = 0; i < STATUS_STAGES; i++) {
        s->stage_max_us[i] = stage_max_us[i];
    }
    for (int i = 0; i < STATUS_STAGE1_PARTS; i++) {
        s->stage1_parts_us[i] = stage1_part_max_us[i];
    }

    /* The power-up self-test: a board that cannot measure altitude must not
     * report itself healthy. */
    s->sensor_ok = fctx && fctx->sensor_type && fctx->sensor_type != SENSOR_PENDING;
    s->fs_ok = fctx && fctx->fs_ok;
    uint16_t diag = fctx ? fctx->diag : 0;
    for (uint16_t bit = 1; bit != 0 && s->n_faults < STATUS_FAULTS_MAX; bit <<= 1) {
        if ((diag & bit) && *flight_diag_name(bit)) {
            s->faults[s->n_faults++] = flight_diag_name(bit);
        }
    }
    s->reset_cause = fctx ? (uint8_t)fctx->reset_cause : 0u;
    s->recovery = fctx ? flight_recovery_text(fctx) : brownout_recovery_name(RECOVER_COLD);
    s->prev_watchdog = boot_prev_watchdog;
    s->prev_stage = boot_prev_stage;
    s->prev_stage_ms = boot_prev_stage_ms;
    s->pyro_refused[0] = fctx && fctx->pyro1_refused;
    s->pyro_refused[1] = fctx && fctx->pyro2_refused;
    s->pyro1_refires = fctx ? (uint8_t)fctx->pyro1_refires : 0u;
    s->main_forced = fctx && fctx->main_forced;
    s->pres_waits = hal_pressure_waits();
    s->pres_rejects = hal_pressure_rejects();
    s->pres_flashed = hal_pressure_flashed();
    s->raw_pa = pp_last_raw_pa();
    s->pad_speed_cms = fctx ? fctx->pad_speed_cms : 0;
    s->ground_degraded = pp_ground_degraded(); /* [GND-CAL-07] */
    s->ground_reseeds = pp_ground_reseeds();
    s->sample_interval_us[0] = hal_pressure_interval_min_us(); /* [SNS-PRES-08] */
    s->sample_interval_us[1] = hal_pressure_interval_max_us();
    s->stamp_lag_max_us = hal_pressure_stamp_lag_max_us();
    s->fit_sigma_mpa = (uint32_t)(pp_sigma_pa() * 1000.0f); /* [SNS-PRES-09] */
    s->mach_lock = fctx && fctx->mach_lock;                 /* [FLT-MACH-02..07] */
    s->mach_flag_ms = fctx && fctx->mach_flag_ms ? fctx->mach_flag_ms - fctx->launch_time : 0u;
    s->peak_lower_bound = fctx && fctx->peak_lower_bound;
    s->usb_attached = fctx && fctx->usb_attached; /* [USB-01..03, USB-08] */
    s->test_mode = fctx && fctx->test_mode;
    s->buzzer_active = buzzer_is_active();

    /* What the buzzer says, or would say off USB, so it can be read rather
     * than counted. */
    beep_reason_t r = beep_reason_for_diag(diag);
    beep_spec_t sp = beep_for(r);
    s->beep = beep_codes_key(r);
    s->beep_kind = beep_codes_kind_name((beep_kind_t)sp.kind);
    s->beep_is_code = sp.kind == BK_CODE;
    s->beep_d1 = sp.d1;
    s->beep_d2 = sp.d2;
}

/* GET /api/status [WEB-API-01, WEB-API-11]: the flight task's half, then what
 * the net task and the board's identity hold. False when the flight task did
 * not take the call. */
static bool status_capture(status_snap_t *s) {
    memset(s, 0, sizeof(*s));
    if (!flight_call(status_capture_flight, s, CALL_MS)) {
        return false;
    }
    s->uptime_ms = to_ms_since_boot(get_absolute_time());
    s->fw_version = FW_VERSION;
    s->board = PYRO_BOARD_NAME;
    s->board_id = BOARD_SHORT_STR;
    switch (board_selftest_result()) {
    case BOARD_SELFTEST_PASS:
        s->board_selftest = 1;
        break;
    case BOARD_SELFTEST_FAIL:
        s->board_selftest = 2;
        break;
    default:
        s->board_selftest = 0;
        break;
    }

    http_work_stats_t ws;
    http_work_stats(&ws);
    s->http_units[0] = ws.units;
    s->http_unit_max_us[0] = ws.max_us;
    s->flash_opens = flash_op_lockouts();
    s->flash_skips = flash_op_timeouts();
    s->flash_refusals = flash_op_refusals();
    s->log_dropped = hal_log_dropped();
    s->flash_erases = flash_op_erases();
    s->flash_programs = flash_op_programs();
    s->flash_deferrals = flash_op_waits();

    const pin_assign_t *pa = pin_store_current();
    snprintf(s->pins_reason, sizeof(s->pins_reason), "%s", pin_store_reason());
    s->pyro_released[0] = pa->pyro1_released;
    s->pyro_released[1] = pa->pyro2_released;
    const char *br_name;
    s->bridge = pin_store_bridge(&s->bridge_ch, &s->bridge_common, &br_name);
    s->pyro_mocked = pyro_release_mocks();
    /* What the claim decided, beside what the assignment asked for: a channel
     * that could not take its pads shows as a disagreement. */
    s->pyro_real[0] = !pyro_release_is_released(1);
    s->pyro_real[1] = !pyro_release_is_released(2);

    snprintf(s->serial, sizeof(s->serial), "%s", board_serial());
    s->serial_assigned = board_serial_assigned();
    strncpy(s->mac_source, board_mac_source(), sizeof(s->mac_source) - 1);
    snprintf(s->hw_id, sizeof(s->hw_id), "%s", board_hw_id());
    s->subnet = board_subnet_octet();
    return true;
}

static void unit_status(int slot) {
    http_conn_t *hc = &conns[slot].h;
    int n = status_json(&conns[slot].status, (char *)hc->work, sizeof(hc->work));
    if (n < 0) {
        http_respond_str(hc, 500, JSON, "{\"error\":\"status exceeds the response buffer\"}");
        return;
    }
    http_respond(hc, 200, JSON, hc->work, (uint32_t)n);
}

enum { HTTP_UNIT_STATUS };

const http_unit_fn http_unit_vt[] = {
    [HTTP_UNIT_STATUS] = unit_status,
};

/* ── Files ────────────────────────────────────────────────────────── */

#define WWW_HEADERS "Cache-Control: no-store, must-revalidate\r\n"

/* Stream a file, framed by its size. The work buffer is littlefs's cache for
 * it when the file is internal. Answers fb_status/fb_body when there is no
 * such file. */
static void serve_file(conn_t *c, const char *path, const char *ctype, const char *extra, uint16_t fb_status,
                       const char *fb_ctype, const char *fb_body) {
    http_conn_t *hc = &c->h;
    if (!fs_take(c)) {
        return;
    }
    if (vfs_open(&c->file, path, VFS_RD, hc->work) == 0) {
        c->file_open = true;
        int32_t size = vfs_size(&c->file);
        if (size >= 0) {
            c->route = R_FILE;
            http_respond_stream(hc, 200, ctype, (uint32_t)size, extra);
            return;
        }
    }
    http_respond_str(hc, fb_status, fb_ctype, fb_body);
}

static uint16_t fill(http_conn_t *hc, uint8_t *dst, uint16_t max) {
    conn_t *c = CONN_OF(hc);
    if (c->route == R_FLOG) {
        return (uint16_t)flog_csv_read(&c->flog.csv, (char *)dst, max);
    }
    if (c->route != R_FILE || !c->file_open) {
        return 0;
    }
    int n = vfs_read(&c->file, dst, max);
    return n > 0 ? (uint16_t)n : 0;
}

/* ── The flight log ───────────────────────────────────────────────── */

static int flog_reader(void *ctx, uint8_t *dst, int n) {
    conn_t *c = (conn_t *)ctx;
    int k = vfs_read(&c->file, dst, (uint32_t)n);
    return k > 0 ? (int)k : 0;
}

/* GET /api/flight.csv [WEB-API-06, DD-062]: the binary log rendered as CSV.
 * Its length is counted a unit at a time (flight_csv_count()) before the head
 * is sent, then the same rendering streams through fill(). */
static void serve_flight_csv(conn_t *c) {
    http_conn_t *hc = &c->h;
    if (!fs_take(c)) {
        return;
    }
    if (vfs_open(&c->file, FLOG_PATH, VFS_RD, hc->work) == 0) {
        c->file_open = true;
        c->route = R_FLOG;
        flog_csv_init(&c->flog.csv, flog_reader, c);
        c->flog.len = 0;
        return;
    }
    serve_file(c, OLD_LOG_PATH, "text/csv", CSV_DISPOSITION, 200, "text/csv", EMPTY_LOG_CSV);
}

/* CSV bytes counted a unit: about a hundred rows. */
#define FLOG_COUNT_STEP 4096

static bool flight_csv_count(conn_t *c) {
    int k = flog_csv_read(&c->flog.csv, NULL, FLOG_COUNT_STEP);
    c->flog.len += (uint32_t)k;
    if (k == FLOG_COUNT_STEP) {
        return false; /* more to count, next unit */
    }
    if (c->flog.len == 0) {
        http_respond_str(&c->h, 200, "text/csv", EMPTY_LOG_CSV);
        return true;
    }
    vfs_rewind(&c->file);
    flog_csv_init(&c->flog.csv, flog_reader, c);
    http_respond_stream(&c->h, 200, "text/csv", c->flog.len, CSV_DISPOSITION);
    return true;
}

/* [WEB-API-12, DD-062] The room the next flight's log has: what is free, and
 * the current log's own, which the next launch replaces, on whichever store
 * the log goes to. Some is kept back for the filesystem's own metadata. */
#define LOG_SPACE_RESERVE_BYTES (16u * 1024u)

static void serve_api_net(http_conn_t *hc);

static void serve_log_space(conn_t *c) {
    http_conn_t *hc = &c->h;
    if (!fs_take(c)) {
        return;
    }
    uint64_t free_b = 0, total_b = 0;
    if (vfs_space(FLOG_PATH, &free_b, &total_b) != 0) {
        http_respond_str(hc, 500, JSON, "{\"error\":\"no filesystem\"}");
        return;
    }
    int32_t log_bytes = vfs_stat_size(FLOG_PATH);
    int64_t room = (int64_t)free_b - LOG_SPACE_RESERVE_BYTES + (log_bytes > 0 ? log_bytes : 0);
    char *buf = (char *)hc->work;
    int n = snprintf(buf, sizeof(hc->work),
                     "{\"bytes_free\":%llu,\"record_bytes\":%u,\"rates_hz\":[1,%lu],\"store\":\"%s\"}",
                     (unsigned long long)(room > 0 ? room : 0), (unsigned)FLOG_SAMPLE_BYTES,
                     (unsigned long)hal_pressure_rate_hz(), vfs_route(FLOG_PATH) == VFS_FAT ? "sd" : "flash");
    http_respond(hc, 200, JSON, buf, (uint32_t)n);
}

#if PYRO_HAS_SD
/* GET /api/sd: the card, its FAT and the driver's counters [DD-075]. */
static void serve_api_sd(conn_t *c) {
    http_conn_t *hc = &c->h;
    sd_stats_t s;
    sd_get_stats(&s);
    uint64_t free_b = 0, total_b = 0;
    bool fat = vfs_sd_mounted() && vfs_space("/", &free_b, &total_b) == 0;
    static const char *types[] = {"none", "v1", "v2_sc", "v2_hc"};
    int n = snprintf((char *)hc->work, sizeof(hc->work),
                     "{\"mounted\":%s,\"type\":\"%s\",\"sectors\":%lu,\"cid\":\"%s\",\"mount_rc\":%lu,"
                     "\"free\":%llu,\"total\":%llu,\"hz\":%lu,\"reads\":%lu,\"writes\":%lu,"
                     "\"sectors_read\":%lu,\"sectors_written\":%lu,\"crc_errors\":%lu,\"cmd_errors\":%lu,"
                     "\"timeouts\":%lu,\"retries\":%lu,\"busy_max_us\":%lu,\"write_max_us\":%lu,"
                     "\"init_r1\":\"%02X %02X %02X %02X %02X %02X\",\"init_r7\":\"%02X%02X%02X%02X\","
                     "\"ocr\":\"%02X%02X%02X%02X\",\"imu_whoami\":%u,\"cmd55\":%u,\"acmd41\":%u,"
                     "\"acmd41_polls\":%lu,\"acmd41_ones\":%lu,\"acmd41_other_ms\":%lu,\"acmd41_other\":%u,"
                     "\"after_r58\":%u,\"after_r0\":%u,\"restarts\":%u,\"fail_ms\":[%u,%u,%u,%u],"
                     "\"init_yields\":%lu}",
                     vfs_sd_mounted() ? "true" : "false", types[sd_type()], (unsigned long)sd_sectors(), sd_cid(),
                     (unsigned long)s.mount_rc, (unsigned long long)(fat ? free_b : 0),
                     (unsigned long long)(fat ? total_b : 0), (unsigned long)s.hz, (unsigned long)s.reads,
                     (unsigned long)s.writes, (unsigned long)s.sectors_read, (unsigned long)s.sectors_written,
                     (unsigned long)s.crc_errors, (unsigned long)s.cmd_errors, (unsigned long)s.timeouts,
                     (unsigned long)s.retries, (unsigned long)s.busy_max_us, (unsigned long)s.write_max_us,
                     s.init_r1[0], s.init_r1[1], s.init_r1[2], s.init_r1[3], s.init_r1[4], s.init_r1[5], s.init_r7[0],
                     s.init_r7[1], s.init_r7[2], s.init_r7[3], s.init_ocr[0], s.init_ocr[1], s.init_ocr[2],
                     s.init_ocr[3], (unsigned)sd_bus_probe_imu(), (unsigned)s.cmd55_first, (unsigned)s.acmd41_first,
                     (unsigned long)s.acmd41_polls, (unsigned long)s.acmd41_ones, (unsigned long)s.acmd41_other_ms,
                     (unsigned)s.acmd41_other, (unsigned)s.after_r58, (unsigned)s.after_r0, (unsigned)s.restarts,
                     (unsigned)s.fail_ms[0], (unsigned)s.fail_ms[1], (unsigned)s.fail_ms[2], (unsigned)s.fail_ms[3],
                     (unsigned long)s.init_yields);
    http_respond(hc, 200, JSON, hc->work, (uint32_t)n);
}

/* GET /api/hr: the high-rate log (hr_log.h). */
static void serve_api_hr(http_conn_t *hc) {
    hr_stats_t s;
    hr_log_get_stats(&s);
    int n = snprintf((char *)hc->work, sizeof(hc->work),
                     "{\"logging\":%s,\"prepared\":%s,\"preparing\":%s,\"card\":%s,\"imu_ok\":%s,\"odr_hz\":%lu,"
                     "\"file\":\"%s\",\"file_bytes\":%lu,\"expanded_bytes\":%lu,\"ring_used\":%lu,\"ring_max\":%lu,"
                     "\"dropped_records\":%lu,\"dropped_bytes\":%lu,\"imu_sets\":%lu,\"imu_reads\":%lu,"
                     "\"imu_overruns\":%lu,\"imu_backlog_max\":%lu,\"imu_read_fails\":%lu,\"pres_records\":%lu,"
                     "\"flight_records\":%lu,\"writes\":%lu,\"write_max_us\":%lu,\"write_errors\":%lu,\"syncs\":%lu,"
                     "\"sync_max_us\":%lu,\"prepare_us\":%lu,\"logs\":%lu,\"reopens\":%lu,\"bytes_total\":%lu,"
                     "\"last_g\":[%d,%d,%d],\"last_a\":[%d,%d,%d]}",
                     s.logging ? "true" : "false", s.prepared ? "true" : "false", s.preparing ? "true" : "false",
                     s.card ? "true" : "false", s.imu_ok ? "true" : "false", (unsigned long)s.odr_hz, s.file,
                     (unsigned long)s.file_bytes, (unsigned long)s.expanded_bytes, (unsigned long)s.ring_used,
                     (unsigned long)s.ring_max, (unsigned long)s.dropped_records, (unsigned long)s.dropped_bytes,
                     (unsigned long)s.imu_sets, (unsigned long)s.imu_reads, (unsigned long)s.imu_overruns,
                     (unsigned long)s.imu_backlog_max, (unsigned long)s.imu_read_fails, (unsigned long)s.pres_records,
                     (unsigned long)s.flight_records, (unsigned long)s.writes, (unsigned long)s.write_max_us,
                     (unsigned long)s.write_errors, (unsigned long)s.syncs, (unsigned long)s.sync_max_us,
                     (unsigned long)s.prepare_us, (unsigned long)s.logs, (unsigned long)s.reopens,
                     (unsigned long)s.bytes_total, s.last.g[0], s.last.g[1], s.last.g[2], s.last.a[0], s.last.a[1],
                     s.last.a[2]);
    http_respond(hc, 200, JSON, hc->work, (uint32_t)n);
}

/* POST /api/sd/bench?kb=N&chunk=B: N kB written to /bench.bin in B-byte
 * writes, timed, then removed. The card's sequential rate and its worst
 * write, on the bench only: the net task does nothing else meanwhile. */
static void apply_sd_bench(http_conn_t *hc, const char *path) {
    const char *q = strstr(path, "kb=");
    uint32_t kb = q ? (uint32_t)strtoul(q + 3, NULL, 10) : 1024u;
    q = strstr(path, "chunk=");
    uint32_t chunk = q ? (uint32_t)strtoul(q + 6, NULL, 10) : 4096u;
    if (chunk < 512u || chunk > sizeof(hc->work) - 64u || kb == 0 || kb > 65536u) {
        http_respond_str(hc, 400, JSON, "{\"error\":\"kb 1-65536, chunk 512 to the work buffer\"}");
        return;
    }
    static FIL f;
    if (!vfs_sd_mounted() || f_open(&f, "/bench.bin", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        http_respond_str(hc, 409, JSON, "{\"error\":\"no card\"}");
        return;
    }
    uint8_t *buf = hc->work;
    for (uint32_t i = 0; i < chunk; i++)
        buf[i] = (uint8_t)i;
    uint32_t total = kb * 1024u, done = 0, worst = 0, errors = 0;
    uint32_t t0 = time_us_32();
    while (done < total) {
        uint32_t n = total - done < chunk ? total - done : chunk;
        UINT put = 0;
        uint32_t a = time_us_32();
        if (f_write(&f, buf, n, &put) != FR_OK || put != n)
            errors++;
        uint32_t d = time_us_32() - a;
        if (d > worst)
            worst = d;
        done += n;
    }
    uint32_t w_us = time_us_32() - t0;
    uint32_t s0 = time_us_32();
    f_close(&f);
    uint32_t close_us = time_us_32() - s0;
    f_unlink("/bench.bin");
    char jb[256];
    int jn = snprintf(jb, sizeof(jb),
                      "{\"bytes\":%lu,\"chunk\":%lu,\"us\":%lu,\"kb_per_s\":%lu,\"worst_write_us\":%lu,"
                      "\"close_us\":%lu,\"errors\":%lu}",
                      (unsigned long)total, (unsigned long)chunk, (unsigned long)w_us,
                      (unsigned long)(w_us ? (uint64_t)total * 1000000u / 1024u / w_us : 0u), (unsigned long)worst,
                      (unsigned long)close_us, (unsigned long)errors);
    http_respond(hc, 200, JSON, jb, (uint32_t)jn);
}
#endif

/* ── Default page if /www/index.html missing ──────────────────────── */

static const char DEFAULT_PAGE[] = "<!DOCTYPE html><html><body><h2>" PYRO_BOARD_NAME "</h2>"
                                   "<p>No web files uploaded. POST files to /www/ to set up the UI.</p>"
                                   "<p><a href=\"/api/status\">Status JSON</a></p></body></html>";

#if PYRO_HAS_LUA
/* GET /api/lua/console: the console ring, drained, and the VM's liveness: a
 * frozen heartbeat under "running" is a VM stuck where the instruction hook
 * cannot reach. The buffers are static, off the net task's stack: it is the
 * only caller. */
static void serve_lua_console(http_conn_t *hc) {
    static char text[900];
    static char esc[1024];
    static char esc_status[192];
    int n = lua_app_console_read(text, sizeof(text) - 1);
    text[n] = '\0';
    json_escape(esc, sizeof(esc), text, n);
    /* A Lua error names its chunk in quotes: [string "check"]:128: */
    const char *st = lua_app_status();
    json_escape(esc_status, sizeof(esc_status), st, (int)strlen(st));
    /* c1_go ahead of c1_seen: a tick dispatched that the VM never took,
     * which otherwise looks the same as a VM stuck mid-tick. */
    uint32_t dbg_go, dbg_seen, dbg_skipped, dbg_hb;
    lua_core1_dispatch_stats(&dbg_go, &dbg_seen, &dbg_skipped, &dbg_hb);
    uint32_t dbg_loc = lua_core1_loc();
    char *body = (char *)hc->work;
    int blen =
        snprintf(body, sizeof(hc->work),
                 "{\"status\":\"%s\",\"heartbeat\":%lu,\"log_written\":%lu,"
                 "\"console_dropped\":%lu,\"log_dropped\":%lu,\"log_refused\":%lu,"
                 "\"log_active\":%s,"
                 "\"c1_state\":%d,\"c1_loc\":%lu,\"c1_busy\":%lu,\"c1_go\":%lu,\"c1_seen\":%lu,"
                 "\"c1_skipped\":%lu,\"c1_ready\":%s,\"c1_flash_ok\":%s,\"stack_free\":%lu,"
                 "\"text\":\"%s\"}",
                 esc_status, (unsigned long)lua_core1_heartbeat(), (unsigned long)lua_app_log_written(),
                 (unsigned long)lua_core1_console_dropped(), (unsigned long)lua_core1_log_dropped(),
                 (unsigned long)hal_log_text_dropped(), hal_log_active() ? "true" : "false", (int)lua_core1_state(),
                 (unsigned long)(dbg_loc & 0xffu), (unsigned long)((dbg_loc >> 8) & 0xffu), (unsigned long)dbg_go,
                 (unsigned long)dbg_seen, (unsigned long)dbg_skipped, lua_core1_ready() ? "true" : "false",
                 lua_core1_flash_ok() ? "true" : "false", (unsigned long)lua_core1_stack_free(), esc);
    if (blen < 0 || blen >= (int)sizeof(hc->work)) {
        http_respond_str(hc, 500, JSON, "{\"error\":\"console exceeds the response buffer\"}");
        return;
    }
    http_respond(hc, 200, JSON, body, (uint32_t)blen);
}
#endif

static void serve_get(conn_t *c) {
    http_conn_t *hc = &c->h;
    const char *path = hc->path;

    if (strcmp(path, "/api/status") == 0) {
        if (!status_capture(&c->status)) {
            http_respond_str(hc, 503, JSON, "{\"error\":\"the flight task did not answer\"}");
            return;
        }
        c->route = R_STATUS;
        http_work_offer((int)(c - conns), HTTP_UNIT_STATUS);
#if PYRO_HAS_LUA
    } else if (strcmp(path, "/api/lua/script") == 0) {
        /* No script yet is a normal state, not an error: the editor should
         * open empty rather than show a 404. */
        serve_file(c, "/" LUA_SCRIPT_PATH, TEXT, NULL, 200, TEXT, "");
    } else if (strcmp(path, "/api/lua/console") == 0) {
        serve_lua_console(hc);
#endif
    } else if (strcmp(path, "/api/beeps") == 0) {
        serve_api_beeps(hc);
    } else if (strcmp(path, "/api/pins/caps") == 0) {
        serve_api_pin_caps(hc);
    } else if (strcmp(path, "/api/pins") == 0) {
        serve_file(c, "/" PIN_STORE_PATH, TEXT, NULL, 404, TEXT, "No pins.ini");
    } else if (strcmp(path, "/api/config") == 0) {
        serve_file(c, "config.ini", TEXT, NULL, 404, TEXT, "No config.ini"); /* [WEB-API-02] */
    } else if (strcmp(path, "/api/flight.csv") == 0) {
        serve_flight_csv(c);
    } else if (strcmp(path, "/api/log/space") == 0) {
        serve_log_space(c);
    } else if (strcmp(path, "/api/net") == 0) {
        serve_api_net(hc);
#if PYRO_HAS_BENCH_FLIGHT
    } else if (strcmp(path, "/api/sim") == 0) {
        serve_api_sim(hc);
#endif
#if PYRO_HAS_SD
    } else if (strcmp(path, "/api/sd") == 0) {
        serve_api_sd(c);
    } else if (strcmp(path, "/api/hr") == 0) {
        serve_api_hr(hc);
#endif
    } else if (strncmp(path, "/api/pressure/trace", 19) == 0 && (path[19] == '\0' || path[19] == '?')) {
        /* Every conversion since ?since=N, in binary (pressure_trace.h), for
         * support/pressure_trace.py. */
        const char *q = strstr(path, "since=");
        uint32_t since = q ? (uint32_t)strtoul(q + 6, NULL, 10) : 0u;
        int n = ptrace_read(since, hc->work, (int)sizeof(hc->work));
        http_respond(hc, 200, "application/octet-stream", hc->work, (uint32_t)n);
    } else if (strcmp(path, "/") == 0) {
        /* no-store: browsers otherwise cache it heuristically and keep
         * showing the previous upload. */
        serve_file(c, "/www/index.html", "text/html", WWW_HEADERS, 200, "text/html", DEFAULT_PAGE);
    } else if (!vfs_path_ok(path)) {
        http_respond_str(hc, 400, TEXT, "a file name is letters, digits, '.', '_' and '-', in '/' segments");
    } else {
        serve_file(c, path, content_type_hdr(path), WWW_HEADERS, 404, TEXT, "Not found");
    }
}

/* ── Uploads ──────────────────────────────────────────────────────────
 *
 * Written to dest + ".part" and renamed over dest only once the last byte is
 * in, so an upload that dies part-way leaves the previous file whole. The
 * part file of one that dies is removed when its connection is released. */

static void part_path(const conn_t *c, char *out, size_t cap) {
    snprintf(out, cap, "%s.part", c->dest);
}

static bool upload_open(conn_t *c) {
    if (strncmp(c->dest, "/www/", 5) == 0) {
        vfs_mkdir("/www");
    }
    char part[HTTP_PATH_MAX + 8];
    part_path(c, part, sizeof(part));
    int err = vfs_open(&c->file, part, VFS_WR, c->h.work);
    if (err != 0) {
        DBG("POST %s FAIL open err=%d", c->dest, err);
        return false;
    }
    c->file_open = true;
    c->file_writing = true;
    return true;
}

static uint16_t upload_body(conn_t *c, const uint8_t *data, uint16_t len) {
    if (!c->file_open && !upload_open(c)) {
        http_respond_str(&c->h, 500, TEXT, "could not open the file for writing");
        return len;
    }
    /* Check every write: an unchecked refusal leaves a file of the right
     * length that is wrong in the middle, with nothing reported. */
    if (vfs_write(&c->file, data, len) != (int)len) {
        DBG("POST %s FAIL write refused", c->dest);
        http_respond_str(&c->h, 500, TEXT, "write failed; file is incomplete, retry");
    }
    return len;
}

static void upload_complete(conn_t *c) {
    /* Close flushes the last partial block, so a refusal there
     * loses the tail of the file as quietly as a refused write does. */
    bool ok = c->file_open && vfs_close(&c->file) == 0;
    c->file_open = false;
    c->file_writing = false;
    char part[HTTP_PATH_MAX + 8];
    part_path(c, part, sizeof(part));
    if (ok) {
        ok = vfs_rename(part, c->dest) == 0;
    }
    if (!ok) {
        vfs_remove(part);
    } else {
        /* The Lua program is configuration: littlefs keeps a copy. */
        vfs_mirror_one(c->dest);
    }
    DBG("POST %s done ok=%d", c->dest, (int)ok);
    if (ok) {
        http_respond_str(&c->h, 201, TEXT, "OK");
    } else {
        http_respond_str(&c->h, 500, TEXT, "close failed; file is incomplete, retry");
    }
}

/* ── OTA ──────────────────────────────────────────────────────────── */

static uint16_t ota_body(conn_t *c, const uint8_t *data, uint16_t len) {
    if (ota_write(data, len) != len) {
        /* A sector the lockout would not write: a truncated image. */
        ota_failed = true;
        http_respond_str(&c->h, 500, TEXT, "OTA aborted: a sector did not write");
    }
    return len;
}

/* ── Small POSTs ──────────────────────────────────────────────────── */

/* POST /api/serial: the board's assigned MAC, into /serial.txt, used from the
 * next boot [DD-072]. Twelve hex digits, unicast, checked before anything
 * touches flash: a board with a bad identity cannot be reached to fix it. */
static bool serial_valid(const char *s) {
    for (int i = 0; i < 12; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    /* Bit 0 of the first octet: multicast. */
    int hi = (s[0] >= '0' && s[0] <= '9') ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
    int lo = (s[1] >= '0' && s[1] <= '9') ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
    return (((hi << 4) | lo) & 0x01) == 0;
}

static void apply_erase(http_conn_t *hc) {
    /* [WEB-API-09, DAT-06] One log slot, which the next launch overwrites;
     * refused at the head while the log is written (fs_take()). */
    int rc = vfs_remove(FLOG_PATH);
    int old = vfs_remove(OLD_LOG_PATH);
    if (rc == VFS_NOENT) {
        rc = old;
    }
    DBG("POST /api/flight/erase rc=%d", rc);
    if (rc == 0 || rc == VFS_NOENT) {
        http_respond_str(hc, 200, JSON, "{\"status\":\"erased\"}");
    } else {
        http_respond_str(hc, 500, JSON, "{\"error\":\"could not erase\"}");
    }
}

#if PYRO_HAS_LUA
/* Validate a script against the LIVE resource set without running a line of
 * it: the editor's as-you-go check. The authoritative one runs on the stored
 * file at boot. */
static void apply_lua_check(http_conn_t *hc) {
    static lua_chk_result_t chk; /* large, and the net task is the only caller */
    lua_app_check((const char *)hc->work, (int)hc->gathered, &flight_get_context()->config, &chk);

    /* The script is done with, so the work buffer takes the answer. */
    char *body = (char *)hc->work;
    const int cap = (int)sizeof(hc->work);
    int blen = snprintf(body, (size_t)cap, "{\"green\":%s,\"items\":[", chk.green ? "true" : "false");
    /* detail is a Lua error, which quotes its chunk name. */
    for (int i = 0; i < chk.count && blen > 0 && blen < cap - 2; i++) {
        char esc_detail[sizeof(chk.items[i].detail) * 2 + 8];
        json_escape(esc_detail, (int)sizeof(esc_detail), chk.items[i].detail, (int)strlen(chk.items[i].detail));
        int n = snprintf(body + blen, (size_t)(cap - blen), "%s{\"kind\":%d,\"detail\":\"%s\"}", i ? "," : "",
                         (int)chk.items[i].kind, esc_detail);
        if (n < 0 || n >= cap - blen) {
            break;
        }
        blen += n;
    }
    if (blen > 0 && blen < cap - 3) {
        blen += snprintf(body + blen, (size_t)(cap - blen), "]}");
    }
    http_respond(hc, 200, JSON, body, (uint32_t)blen);
}
#endif

/* ── Routing ──────────────────────────────────────────────────────── */

#if PYRO_HAS_SD
static void apply_hr_start(http_conn_t *hc, const char *path) {
    const char *q = strstr(path, "odr=");
    if (q) {
        uint32_t hz = (uint32_t)strtoul(q + 4, NULL, 10);
        lsm6ds3_odr_t o = hz >= 1660  ? LSM6DS3_ODR_1660
                          : hz >= 833 ? LSM6DS3_ODR_833
                          : hz >= 416 ? LSM6DS3_ODR_416
                          : hz >= 208 ? LSM6DS3_ODR_208
                                      : LSM6DS3_ODR_104;
        hr_log_set_odr(o);
    }
    bool ok = hr_log_start("bench");
    http_respond_str(hc, ok ? 200 : 409, JSON, ok ? "{\"status\":\"logging\"}" : "{\"error\":\"no card\"}");
}

/* Bring the card up again, and mount it: a card inserted after boot, or one
 * that failed then. ?crc=0 leaves CMD59 off; ?timeout=ms and ?restarts=n
 * hold a card that resets on the rail to be measured; ?hz= sets the data
 * clock. */
static void apply_sd_init(http_conn_t *hc, const char *path) {
    sd_set_crc(strstr(path, "crc=0") == NULL);
    const char *to = strstr(path, "timeout=");
    sd_set_init_timeout_ms(to ? (uint32_t)strtoul(to + 8, NULL, 10) : 1000u);
    const char *gap = strstr(path, "gap=");
    sd_set_poll_gap_ms(gap ? (uint32_t)strtoul(gap + 4, NULL, 10) : 0u);
    const char *rs = strstr(path, "restarts=");
    sd_set_init_restarts(rs ? (uint32_t)strtoul(rs + 9, NULL, 10) : 3u);
    const char *hz = strstr(path, "hz=");
    sd_set_data_hz(hz ? (uint32_t)strtoul(hz + 3, NULL, 10) : BOARD_SD_SPI_HZ);
    int rc = sd_start();
    char jb[64];
    int jn = snprintf(jb, sizeof(jb), "{\"rc\":%d,\"mounted\":%s}", rc, vfs_sd_mounted() ? "true" : "false");
    http_respond(hc, 200, JSON, jb, (uint32_t)jn);
}

static void apply_sd_idle(http_conn_t *hc, const char *path) {
    const char *q = strstr(path, "ms=");
    bool ok = sd_clock_idle(q ? (uint32_t)strtoul(q + 3, NULL, 10) : 2000u);
    http_respond_str(hc, ok ? 200 : 503, JSON, ok ? "{\"status\":\"clocked\"}" : "{\"error\":\"bus\"}");
}
#endif

/* The POSTs that act on the path alone, answered at the head. */
static void apply_path_post(conn_t *c, route_t route) {
    http_conn_t *hc = &c->h;
    switch (route) {
    case R_REBOOT: /* [WEB-API-05] */
        DBG("POST /api/reboot");
        c->reboot_when_sent = true;
        http_respond_str(hc, 200, TEXT, "Rebooting");
        break;
    case R_TEST_MODE_ON:
    case R_TEST_MODE_OFF:
        apply_api_test_mode(hc, route == R_TEST_MODE_ON);
        break;
#if PYRO_HAS_BENCH_FLIGHT
    case R_SIM_FLIGHT:
        apply_sim_flight(hc, hc->path);
        break;
    case R_SIM_STOP: {
        bool ok = flight_call(sim_stop_call, NULL, CALL_MS);
        http_respond_str(hc, ok ? 200 : 503, JSON, ok ? "{\"status\":\"stopped\"}" : "{\"error\":\"busy\"}");
        break;
    }
#endif
#if PYRO_HAS_SD
    case R_SD_BENCH:
        apply_sd_bench(hc, hc->path);
        break;
    case R_SD_INIT:
        apply_sd_init(hc, hc->path);
        break;
    case R_SD_IDLE:
        apply_sd_idle(hc, hc->path);
        break;
    case R_HR_START:
        apply_hr_start(hc, hc->path);
        break;
    case R_HR_STOP:
        hr_log_stop();
        http_respond_str(hc, 200, JSON, "{\"status\":\"stopping\"}");
        break;
#endif
    default:
        /* R_ERASE: answered once the body, if any, is discarded. */
        c->route = route;
        break;
    }
}

/* [WEB-API-04, OTA-01] Per transfer: left latched, a failure would abort
 * every later upload at its first byte. */
static void ota_begin(conn_t *c) {
    ota_conn = c;
    ota_offset = 0;
    ota_buf_fill = 0;
    ota_failed = false;
    pfb_started = false;
}

static void route_post(conn_t *c) {
    http_conn_t *hc = &c->h;
    const char *path = hc->path;

    const post_route_t *r = find_post_route(path);
    if (!r) {
        http_respond_str(hc, 404, TEXT, "Not found");
        return;
    }
    if (r->route == R_UPLOAD && !vfs_path_ok(path)) {
        http_respond_str(hc, 400, TEXT, "a file name is letters, digits, '.', '_' and '-', in '/' segments");
        return;
    }
    if (r->fs && !fs_take(c)) {
        return;
    }
    if (r->body == BODY_NONE) {
        apply_path_post(c, r->route);
        return;
    }
    if (hc->content_length == 0) {
        http_respond_str(hc, 400, TEXT, "this request needs a body");
        return;
    }
    if (r->route == R_OTA) {
        if (ota_conn && ota_conn != c) {
            http_respond_str(hc, 409, TEXT, "another update is in progress");
            return;
        }
        if (!ota_image_fits(hc->content_length, OTA_SLOT_BYTES)) {
            http_respond_str(hc, 413, TEXT, "larger than the download slot");
            return;
        }
    }
    c->route = r->route;
    if (r->body == BODY_GATHER) {
        http_gather(hc, r->gather);
    } else {
        http_stream(hc);
    }
    if (r->route == R_OTA) {
        ota_begin(c);
    } else if (r->route == R_UPLOAD) {
#if PYRO_HAS_LUA
        if (strcmp(path, "/api/lua/script") == 0) {
            snprintf(c->dest, sizeof(c->dest), "/%s", LUA_SCRIPT_PATH);
        } else
#endif
        {
            snprintf(c->dest, sizeof(c->dest), "%s", path);
        }
        DBG("POST %s cl=%lu", c->dest, (unsigned long)hc->content_length);
    }
}

static void on_head(http_conn_t *hc) {
    conn_t *c = CONN_OF(hc);
    /* A page on another site, by a rebound name or a form. */
    uint16_t refusal = http_origin_refusal(hc, board_subnet_octet());
    if (refusal) {
        http_respond_str(hc, refusal, TEXT,
                         refusal == 400 ? "a request names its Host" : "not from this board's own page");
        return;
    }
    if (strcmp(hc->method, "POST") == 0) {
        route_post(c);
    } else {
        serve_get(c);
    }
}

static uint16_t on_body(http_conn_t *hc, const uint8_t *data, uint16_t len) {
    conn_t *c = CONN_OF(hc);
    if (c->route == R_OTA) {
        if (!pfb_started) {
            pfb_started = true;
            if (flash_op(pfb_begin_op, NULL) != 0) {
                ota_failed = true;
            }
        }
        return ota_body(c, data, len);
    }
    return upload_body(c, data, len);
}

static bool on_complete(http_conn_t *hc) {
    conn_t *c = CONN_OF(hc);
    char *body = (char *)hc->work;
    switch (c->route) {
    case R_STATUS:
        return false;
    case R_FLOG:
        return flight_csv_count(c);
    case R_BEEP_PLAY:
        apply_api_beep_play(hc, body);
        return true;
#if PYRO_HAS_LUA
    case R_LUA_CHECK:
        apply_lua_check(hc);
        return true;
#endif
    case R_SERIAL:
        if (hc->gathered != 12 || !serial_valid(body)) {
            http_respond_str(hc, 400, TEXT, "expected 12 hex digits, unicast (first octet even)");
            return true;
        }
        break;
    default:
        break;
    }

    switch (c->route) {
    case R_UPLOAD:
        upload_complete(c);
        break;
    case R_OTA: /* [WEB-API-04] */
        if (ota_failed || !ota_flush() || flash_op(pfb_valid_op, NULL) != 0) {
            http_respond_str(hc, 500, TEXT, "OTA failed: the image did not write");
            break;
        }
        c->reboot_when_sent = true;
        http_respond_str(hc, 200, TEXT, "OTA OK, rebooting...");
        break;
    case R_CONFIG:
        DBG("POST /api/config cl=%lu", (unsigned long)hc->gathered);
        apply_api_config(hc, body);
        break;
    case R_PINS:
        apply_api_pins(hc, body);
        break;
    case R_BEEPS:
        apply_api_beeps(hc, body);
        break;
    case R_SERIAL: {
        int rc = hal_fs_write_file("serial.txt", body, 12);
        if (rc == HAL_FS_LOCKED) {
            respond_fs_locked(hc);
        } else if (rc != 0) {
            http_respond_str(hc, 500, TEXT, "write failed");
        } else {
            http_respond_str(hc, 200, TEXT, "OK, reboot to apply");
        }
        break;
    }
    case R_ERASE:
        apply_erase(hc);
        break;
    default:
        http_respond_str(hc, 404, TEXT, "Not found");
        break;
    }
    return true;
}

static const http_handlers_t handlers = {
    .common_headers = COMMON_HDRS,
    .on_head = on_head,
    .on_body = on_body,
    .on_complete = on_complete,
    .fill = fill,
};

/* ── The lwIP adapter ─────────────────────────────────────────────── */

extern volatile uint32_t net_http_accept;
extern volatile uint32_t net_http_err;
extern volatile uint32_t net_conn_full;
extern volatile uint32_t net_rx_count, net_rx_drop, net_tx_fail, net_tx_ok, net_tx_held;
extern volatile uint32_t net_usb_events[4];

/* [WEB-API-13] What the transport refused. */
static uint32_t net_accept_refused, net_write_fails, net_idle_aborts, net_last_accept_ms;

static void net_pool(net_pool_t *o, const struct stats_mem *m) {
    o->used = m->used;
    o->max = m->max;
    o->err = m->err;
}

/* GET /api/net [WEB-API-13]: lwIP's pools, TCP's connections by state, and
 * what the transport refused (net_stats.h). */
static void serve_api_net(http_conn_t *hc) {
    net_snap_t s;
    memset(&s, 0, sizeof(s));
    net_pool(&s.heap, &lwip_stats.mem);
    net_pool(&s.tcp_pcb, lwip_stats.memp[MEMP_TCP_PCB]);
    net_pool(&s.tcp_seg, lwip_stats.memp[MEMP_TCP_SEG]);
    net_pool(&s.pbuf_pool, lwip_stats.memp[MEMP_PBUF_POOL]);
    s.tcp_xmit = lwip_stats.tcp.xmit;
    s.tcp_recv = lwip_stats.tcp.recv;
    s.tcp_drop = lwip_stats.tcp.drop;
    s.tcp_memerr = lwip_stats.tcp.memerr;
    s.icmp_recv = lwip_stats.icmp.recv;
    s.icmp_xmit = lwip_stats.icmp.xmit;
    struct tcp_pcb *const lists[2] = {tcp_active_pcbs, tcp_tw_pcbs};
    for (int i = 0; i < 2; i++) {
        for (struct tcp_pcb *p = lists[i]; p; p = p->next) {
            if ((unsigned)p->state < sizeof(s.states) / sizeof(s.states[0]))
                s.states[p->state]++;
            if (p->snd_queuelen > s.sndq_max)
                s.sndq_max = p->snd_queuelen;
            if (p->nrtx > s.nrtx_max)
                s.nrtx_max = p->nrtx;
        }
    }
    s.accepts = net_http_accept;
    s.accept_refused = net_accept_refused;
    s.write_fails = net_write_fails;
    s.conn_full = net_conn_full;
    s.http_err = net_http_err;
    s.idle_aborts = net_idle_aborts;
    s.rx_frames = net_rx_count;
    s.rx_drops = net_rx_drop;
    s.tx_sent = net_tx_ok;
    s.tx_held = net_tx_held;
    s.tx_refused = net_tx_fail;
    for (int i = 0; i < 4; i++)
        s.usb[i] = net_usb_events[i];
    s.last_accept_ms = net_last_accept_ms;
    int n = net_json(&s, (char *)hc->work, sizeof(hc->work));
    http_respond(hc, 200, JSON, hc->work, (uint32_t)n);
}

static int slot_of(const conn_t *c) {
    return (int)(c - conns);
}

static void conn_release(conn_t *c) {
    http_work_cancel(slot_of(c));
    if (c->file_open) {
        /* An upload that did not finish: its part file goes, and dest keeps
         * whatever it held before. */
        vfs_close(&c->file);
        if (c->file_writing) {
            char part[HTTP_PATH_MAX + 8];
            part_path(c, part, sizeof(part));
            vfs_remove(part);
        }
    }
    c->file_open = false;
    c->file_writing = false;
    if (c->fs_held) {
        hal_fs_leave();
        c->fs_held = false;
    }
    if (ota_conn == c) {
        ota_conn = NULL;
    }
    if (c->reboot_when_sent) {
        /* The flight task arms a 100 ms watchdog; the net task meanwhile
         * sends the reply lwIP now holds. */
        extern volatile uint8_t pending_reset;
        pending_reset = 2;
    }
    if (c->link) {
        c->link->conn = NULL;
    }
    c->link = NULL;
}

static void conn_bind(conn_t *c, link_t *l) {
    http_conn_init(&c->h);
    c->route = R_NONE;
    c->file_open = false;
    c->file_writing = false;
    c->fs_held = false;
    c->reboot_when_sent = false;
    c->dest[0] = '\0';
    c->link = l;
    l->conn = c;
}

/* Every byte not yet acknowledged to lwIP, acknowledged and dropped. Without
 * this tcp_close() sends RST, and a client can lose the response it has not
 * read yet. Anything arriving after the close is swallowed by lwIP. */
static void link_drop_rx(link_t *l) {
    uint32_t n = 0;
    if (l->conn) {
        n += net_ring_readable(&l->conn->h.rx);
        net_ring_clear(&l->conn->h.rx);
    }
    if (l->pending) {
        n += l->pending->tot_len;
        pbuf_free(l->pending);
        l->pending = NULL;
    }
    while (n > 0 && l->pcb) {
        u16_t k = n > 0xFFFFu ? 0xFFFFu : (u16_t)n;
        tcp_recved(l->pcb, k);
        n -= k;
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
static void on_err(void *arg, err_t err);

static void link_attach(link_t *l) {
    tcp_arg(l->pcb, l);
    tcp_recv(l->pcb, on_recv);
    tcp_sent(l->pcb, on_sent);
    tcp_err(l->pcb, on_err);
}

static void link_detach(link_t *l) {
    tcp_arg(l->pcb, NULL);
    tcp_recv(l->pcb, NULL);
    tcp_sent(l->pcb, NULL);
    tcp_err(l->pcb, NULL);
}

static void link_free(link_t *l) {
    if (l->conn) {
        conn_release(l->conn);
    }
    if (l->pending) {
        pbuf_free(l->pending);
        l->pending = NULL;
    }
    l->pcb = NULL;
}

/* Returns false when lwIP could not take the FIN yet; asked again next pass. */
static bool link_close(link_t *l) {
    link_drop_rx(l);
    link_detach(l);
    if (tcp_close(l->pcb) != ERR_OK) {
        link_attach(l);
        return false;
    }
    link_free(l);
    return true;
}

static void link_abort(link_t *l) {
    net_http_err++;
    link_detach(l);
    tcp_abort(l->pcb);
    link_free(l);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    link_t *l = (link_t *)arg;
    if (!l) {
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p) {
        l->fin = true;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return ERR_OK;
    }
    if (l->pending) {
        pbuf_cat(l->pending, p);
    } else {
        l->pending = p;
    }
    l->last_ms = hal_time_ms();
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb;
    (void)len;
    link_t *l = (link_t *)arg;
    if (l) {
        l->last_ms = hal_time_ms();
    }
    return ERR_OK;
}

/* lwIP has already freed the pcb. */
static void on_err(void *arg, err_t err) {
    (void)err;
    link_t *l = (link_t *)arg;
    if (l) {
        net_http_err++;
        link_free(l);
    }
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) {
        net_accept_refused++;
        return ERR_VAL;
    }
    link_t *l = NULL;
    for (int i = 0; i < LINK_POOL_SIZE; i++) {
        if (!links[i].pcb) {
            l = &links[i];
            break;
        }
    }
    if (!l) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    net_http_accept++;
    net_last_accept_ms = hal_time_ms();
    *l = (link_t){.pcb = pcb, .seq = ++link_seq, .last_ms = net_last_accept_ms};
    tcp_nagle_disable(pcb);
    link_attach(l);
    return ERR_OK;
}

/* Free exchanges go to links that have sent something, in the order they
 * connected. */
static void attach_waiting(void) {
    for (;;) {
        conn_t *c = NULL;
        for (int i = 0; i < CONN_POOL_SIZE && !c; i++) {
            if (!conns[i].link) {
                c = &conns[i];
            }
        }
        link_t *oldest = NULL;
        for (int i = 0; i < LINK_POOL_SIZE; i++) {
            link_t *l = &links[i];
            if (l->pcb && !l->conn && l->pending && (!oldest || (int32_t)(l->seq - oldest->seq) < 0)) {
                oldest = l;
            }
        }
        if (!oldest) {
            return;
        }
        if (!c) {
            if (!oldest->counted_wait) {
                net_conn_full++;
                oldest->counted_wait = true;
            }
            return;
        }
        conn_bind(c, oldest);
    }
}

/* Bytes between lwIP and the rings, and the decision to close. No handler
 * runs here. */
static void transport_link(link_t *l, uint32_t now) {
    struct tcp_pcb *pcb = l->pcb;
    conn_t *c = l->conn;
    if (!c) {
        if (l->fin && !l->pending) {
            link_close(l);
        } else if (!l->pending && now - l->last_ms > HTTP_IDLE_MS) {
            link_abort(l); /* a socket opened speculatively and never used */
        }
        return;
    }
    http_conn_t *hc = &c->h;

    /* [WEB-API-08, DD-058] The log takes the filesystem at launch. A request
     * that took it before then lets go now, so the log can mount; its client
     * sees the connection reset. */
    if (c->fs_held && hal_fs_locked()) {
        link_abort(l);
        return;
    }

    while (l->pending && net_ring_writable(&hc->rx) > 0) {
        uint8_t *p;
        uint16_t span = net_ring_write_span(&hc->rx, &p);
        uint16_t n = pbuf_copy_partial(l->pending, p, span, 0);
        net_ring_commit(&hc->rx, n);
        l->pending = pbuf_free_header(l->pending, n);
    }
    hc->rx_eof = l->fin && !l->pending;

    /* [WEB-HTTP-03] The window reopens by what the parser took, not by what
     * arrived: a request waiting on storage holds its sender back. */
    uint32_t used = http_conn_take_consumed(hc);
    if (used > 0) {
        l->last_ms = now;
    }
    while (used > 0) {
        u16_t k = used > 0xFFFFu ? 0xFFFFu : (u16_t)used;
        tcp_recved(pcb, k);
        used -= k;
    }

    bool wrote = false;
    while (net_ring_readable(&hc->tx) > 0) {
        const uint8_t *p;
        uint16_t span = net_ring_read_span(&hc->tx, &p);
        u16_t room = tcp_sndbuf(pcb);
        uint16_t n = span < room ? span : room;
        if (n == 0)
            break;
        if (tcp_write(pcb, p, n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            net_write_fails++;
            break;
        }
        net_ring_discard(&hc->tx, n);
        wrote = true;
    }
    if (wrote) {
        tcp_output(pcb);
        l->last_ms = now;
    }

    if (hc->failed) {
        link_abort(l);
    } else if (http_conn_done(hc) && net_ring_readable(&hc->tx) == 0) {
        link_close(l);
    } else if (now - l->last_ms > HTTP_IDLE_MS) {
        net_idle_aborts++;
        link_abort(l);
    }
}

void http_server_transport(void) {
    uint32_t now = hal_time_ms();
    attach_waiting();
    for (int i = 0; i < LINK_POOL_SIZE; i++) {
        if (links[i].pcb) {
            transport_link(&links[i], now);
        }
    }
}

void http_server_period(void) {
    http_work_period();
}

bool http_server_work(int32_t remaining_us) {
    bool runnable[CONN_POOL_SIZE];
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        runnable[i] = conns[i].link && http_conn_wants_service(&conns[i].h);
    }
    uint8_t unit;
    int i = http_work_next(runnable, remaining_us, &unit);
    if (i < 0) {
        return false;
    }
    uint32_t t0 = time_us_32();
    if (unit != HTTP_UNIT_NONE) {
        flash_op_crumb(81);
        http_unit_vt[unit](i);
    } else {
        flash_op_crumb(80);
        http_conn_service(&conns[i].h, &handlers);
    }
    flash_op_crumb(82);
    http_work_note(time_us_32() - t0);
    return true;
}

/* Accepts are not limited by lwIP (TCP_LISTEN_BACKLOG is off): on_accept()
 * refuses once every link is in use. */
void http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        DBG("listen: no pcb");
        return;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        DBG("listen: port 80 refused");
        tcp_close(pcb);
        return;
    }
    struct tcp_pcb *listener = tcp_listen(pcb);
    if (!listener) {
        DBG("listen: no listening pcb");
        tcp_close(pcb);
        return;
    }
    tcp_accept(listener, on_accept);
}
