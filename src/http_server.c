/*
 * The HTTP server: files from littlefs, the API, uploads and OTA.
 *
 * Two halves. The routes below answer one request on an http_conn_t, which
 * parses from an rx ring and writes to a tx ring (http_conn.h) and never sees
 * a segment. The lwIP adapter at the bottom moves bytes between lwIP and
 * those rings: it queues what arrives, hands it over as the parser consumes
 * it -- which is also what reopens the TCP window -- and feeds tx to lwIP as
 * its send buffer allows. lwIP callbacks only queue; all the work happens in
 * http_server_service(), from the main loop.
 */
#include "lwip/tcp.h"
#include "board_id.h"
#include "board_if.h"
#include <string.h>
#include <stdio.h>
#include <lfs.h>
#include <pico/stdlib.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <pico_fota_bootloader/core.h>
#include "device_status.h"
#include "version.h"
#include "flight_states.h"
#include "pressure_processing.h"

/* Defined by src/lua/lua_core1.c; weak no-ops in littlefs_driver.c when
 * Lua is not linked. */
#if PYRO_HAS_LUA
#include "lua_app.h"
#include "lua_core1.h"
#endif

#include "flash_window.h"
#include "pin_store.h"
#include "brownout.h"
#include "beep_store.h"
#include "pin_caps.h"
#include "buzzer.h"
#include "pyro_release.h"
#include "http_conn.h"

extern uint32_t hal_time_ms(void);

extern void hal_telemetry_send(const char *sentence);
#define DBG(fmt, ...)                                                                                                  \
    do {                                                                                                               \
        char _b[128];                                                                                                  \
        snprintf(_b, sizeof(_b), "HTTP: " fmt "\r\n", ##__VA_ARGS__);                                                  \
        hal_telemetry_send(_b);                                                                                        \
    } while (0)

extern const char *pressure_sensor_name(void);
/* hal_common.c: sensor reads deferred for an unfinished conversion, and
 * readings refused as impossible. The second must stay at zero. */
extern uint32_t hal_pressure_waits(void);
extern uint32_t hal_pressure_rejects(void);

#define CORS_HDR "Access-Control-Allow-Origin: *\r\n"

extern const struct lfs_config lfs_pico_flash_config;
extern const struct lfs_file_config lfs_pico_file_config;

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

typedef enum {
    R_NONE,
    R_FILE,   /* GET of a littlefs file, streamed by fill() */
    R_UPLOAD, /* POST of a file: /www/..., the Lua program */
    R_OTA,
    R_CONFIG,
    R_PINS,
    R_BEEPS,
    R_SERIAL,
    R_ERASE,
    R_BEEP_PLAY,
    R_LUA_CHECK,
} route_t;

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
    http_conn_t h; /* MUST be first: the handlers cast back */
    link_t *link;  /* NULL: free */
    route_t route;
    lfs_t lfs;
    lfs_file_t file;
    struct lfs_file_config file_cfg;
    bool lfs_mounted;
    bool file_open;
    bool file_writing;
    bool holding;          /* this exchange holds the flash window */
    bool deferral_counted; /* one deferral per wait, not per pass */
    bool reboot_when_sent;
    char dest[HTTP_PATH_MAX];
} conn_t;

static link_t links[LINK_POOL_SIZE];
static conn_t conns[CONN_POOL_SIZE];
static uint32_t link_seq;
static conn_t *ota_conn; /* the OTA state below is one image at a time */

/* A flash-writing request asks this before each step. The hold stops core0
 * dispatching core1, so the window opens within a period; until then the
 * request waits with its bytes in the ring, and flow control does the rest. */
static bool flash_ready(conn_t *c) {
    flash_window_hold(hal_time_ms());
    c->holding = true;
    if (flash_window_is_open()) {
        c->deferral_counted = false;
        return true;
    }
    if (!c->deferral_counted) {
        flash_window_deferred();
        c->deferral_counted = true;
    }
    return false;
}

static void release_window(conn_t *c) {
    if (c->holding) {
        flash_window_release();
        c->holding = false;
    }
}

/* ── OTA firmware update state ────────────────────────────────────── */

/* Download slot flash offset (from linker symbols) */
extern uint32_t __FLASH_DOWNLOAD_SLOT_START;
#define OTA_SLOT_OFF ((uint32_t) & __FLASH_DOWNLOAD_SLOT_START - XIP_BASE)

static uint8_t ota_buf[FLASH_SECTOR_SIZE] __attribute__((aligned(FLASH_PAGE_SIZE)));
static uint32_t ota_offset; /* bytes written so far */
static uint16_t ota_buf_fill;
static bool ota_failed;
static bool pfb_started; /* the rollback mark is a flash write: done in the window */

/* Returns false with nothing written when the window is shut; lwIP
 * redelivers the same bytes, so a refusal costs latency and never data.
 *
 * Do not wait for the window here. This runs inside an lwIP callback, itself
 * inside core0's slack loop, so only lua_app_service() can release core0 and
 * a spin here stops core0 reaching it. Core1's unbounded startup holds
 * flash_ok false for up to five seconds. */
static bool ota_flush(void) {
    if (ota_buf_fill == 0)
        return true;
    if (!flash_window_is_open()) {
        flash_window_refused();
        return false;
    }
    /* pad to page alignment */
    while (ota_buf_fill & (FLASH_PAGE_SIZE - 1))
        ota_buf[ota_buf_fill++] = 0xFF;
    uint32_t addr = OTA_SLOT_OFF + ota_offset;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(addr, FLASH_SECTOR_SIZE);
    flash_range_program(addr, ota_buf, ota_buf_fill);
    restore_interrupts(ints);
    ota_offset += FLASH_SECTOR_SIZE;
    ota_buf_fill = 0;
    return true;
}

/* Short when the sector filled and the window would not take it. */
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

/* Minimal JSON string escaping: quote, backslash and newline, dropping the
 * rest of the control range. Truncates rather than overflowing. */
static void json_escape(char *out, int out_sz, const char *in, int len) {
    int j = 0;
    for (int i = 0; i < len && j < out_sz - 8; i++) {
        char ch = in[i];
        if (ch == '"' || ch == '\\') {
            out[j++] = '\\';
            out[j++] = ch;
        } else if (ch == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if ((unsigned char)ch >= 0x20) {
            out[j++] = ch;
        }
    }
    out[j] = '\0';
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

/* [PYR-SAFE-04] A browser must not reboot, reflash or write the flash of a
 * board that is flying. Every POST changes something; the one GET that does
 * is the capture, which drives the firing bus. */
static bool refused_in_flight(const char *method, const char *path) {
    if (!flight_in_progress()) {
        return false;
    }
    return strcmp(method, "POST") == 0 || strncmp(path, "/api/capture", 12) == 0;
}

#define FLIGHT_LOG_PATH "flight_log.csv"
#define FLIGHT_ERASE_PATH "/api/flight/erase"

/* ── API handlers ─────────────────────────────────────────────────── */

/* Indexed by flight_state_t, so the order here follows the enum -- including
 * BOOT_SENSOR and FAULT, which are appended there to keep the numbers that
 * reach the flight log and telemetry stable. */
static const char *state_names[] = {"BOOT_SETTLE", "BOOT_CONTINUITY", "BOOT_CALIBRATE", "PAD_IDLE",
                                    "ASCENT",      "FALLING",         "DROGUE_DESCENT", "CHUTE_DESCENT",
                                    "LANDED",      "BOOT_SENSOR",     "FAULT"};
#define STATE_NAME_COUNT ((int)(sizeof(state_names) / sizeof(state_names[0])))

/* Main-loop pacing counters (main_hardware.c). Reported so the budget a
 * board declares in board.cmake can be checked against what it actually
 * does, rather than being taken on trust. */
extern volatile uint32_t loop_count, loop_max_us, loop_overruns, loop_late_max_us;
extern volatile uint32_t stage_max_us[];

#include "board_identity.h"

/* POST /api/beeps/play : play one sound, once.
 *
 * A beep editor that only shows numbers asks an operator to choose sounds they
 * will identify by ear, from a form. This is how they hear one first.
 *
 * Takes a kind and, for a counted code, its beeps -- because a chirp is not a
 * number. Writes no flash, so it needs no window. PAD_IDLE only: the buzzer is
 * the flight software's voice and a browser must not talk over a launch. */
static void apply_api_beep_play(http_conn_t *hc, const char *body) {
    extern flight_state_t flight_get_state(void);

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
        /* MK1A fits none. Answering "playing" would be a lie the operator
         * could only detect by listening to silence. */
        jn = snprintf(jb, sizeof(jb), "{\"error\":\"this board has no buzzer fitted\"}");
        status = 409;
    } else {
        /* Once, with no gap: an audition is a sample, not a state. */
        buzzer_play_spec(&sp, 0, 1);
        jn = snprintf(jb, sizeof(jb), "{\"status\":\"playing\",\"kind\":\"%s\",\"d1\":%u,\"d2\":%u}",
                      beep_codes_kind_name((beep_kind_t)sp.kind), (unsigned)sp.d1, (unsigned)sp.d2);
        status = 200;
    }

    http_respond(hc, status, JSON, jb, (uint32_t)jn);
}

/* POST /api/test_mode/on, /api/test_mode/off [USB-08]. Held in RAM, so a
 * reboot ends it. Refused in flight by refused_in_flight(), and ignored there
 * by the flight layer as well. */
#define TEST_MODE_ON_PATH "/api/test_mode/on"
#define TEST_MODE_OFF_PATH "/api/test_mode/off"

static void apply_api_test_mode(http_conn_t *hc, bool on) {
    flight_context_t *ctx = flight_get_context();
    if (!ctx) {
        http_respond_str(hc, 503, JSON, "{\"error\":\"not ready\"}");
        return;
    }
    flight_set_test_mode(ctx, on, hal_time_ms());
    http_respond_str(hc, 200, JSON, ctx->test_mode ? "{\"test_mode\":true}" : "{\"test_mode\":false}");
}

/* ── GET /api/beeps ───────────────────────────────────────────────
 *
 * The outcomes, their meanings, the three personalities and which is active.
 * Same vocabulary-travels-with-the-data shape as /api/pins/caps, so app.js
 * holds no copy: adding an outcome or a pattern kind grows a row without
 * touching the UI. */
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

/* Apply a complete config.ini body and answer. Runs inside the flash window. */
static void apply_api_config(http_conn_t *hc, char *cfgbuf) {
    extern flight_state_t flight_get_state(void);
    extern flight_context_t *flight_get_context(void);
    extern int flight_config_reload(flight_context_t *);
    flight_state_t state = flight_get_state();

    if (state != PAD_IDLE) {
        /* Reject config changes - device not ready */
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
        /* Merge, never replace (REQUIREMENTS.md CFG-06).
         *
         * The body is a PARTIAL config: the Config tab posts eight keys
         * and the Lua tab posts only the lua_* ones. Writing it verbatim
         * left config.ini holding just those keys, and hal_config_load()
         * starts from config_set_defaults(), so every field the other tab
         * owns reverted. Saving config wiped the Lua pin roles and saving
         * Lua reset the rocket id, name and both pyro modes.
         *
         * Parsing over the running config and re-serialising also gives
         * CFG-08 for free: config_parse_ini() ignores keys it does not
         * know, so an unknown key neither lands nor destroys anything. */
        config_t merged = flight_get_context()->config;
        config_parse_ini(cfgbuf, &merged);
        char cfgout[512];
        int cfgn = config_serialize_ini(&merged, cfgout, (int)sizeof(cfgout));
        if (cfgn <= 0) {
            /* Does not fit what hal_config_load() can read back, so
             * writing it would produce a file the board cannot parse. */
            http_respond_str(hc, 500, JSON, "{\"error\":\"Merged config exceeds the 512-byte budget\"}");
            return;
        }

        /* Safe to update: write to flash and reload */
        lfs_t lfs;
        if (lfs_mount(&lfs, &lfs_pico_flash_config) == LFS_ERR_OK) {
            lfs_file_t f;
            if (lfs_file_opencfg(&lfs, &f, "config.ini", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                                 &lfs_pico_file_config) == LFS_ERR_OK) {
                lfs_ssize_t written = lfs_file_write(&lfs, &f, cfgout, cfgn);
                int close_err = lfs_file_close(&lfs, &f);
                int unmount_err = lfs_unmount(&lfs);

                DBG("POST /api/config write=%d close=%d unmount=%d", (int)written, close_err, unmount_err);

                /* Reload config into running system */
                flight_context_t *ctx = flight_get_context();
                int reload_result = flight_config_reload(ctx);

                if (reload_result == 0) {
                    DBG("POST /api/config OK (applied)");
                    http_respond_str(hc, 200, JSON, "{\"status\":\"ok\",\"applied\":true}");
                } else {
                    DBG("POST /api/config WARN reload_result=%d", reload_result);
                    http_respond_str(hc, 500, JSON,
                                     "{\"error\":\"Config saved but reload failed\",\"reboot_required\":true}");
                }
            } else {
                DBG("POST /api/config FAIL file_open");
                lfs_unmount(&lfs);
                http_respond_str(hc, 500, TEXT, "File open failed");
            }
        } else {
            DBG("POST /api/config FAIL lfs_mount");
            http_respond_str(hc, 500, TEXT, "Mount failed");
        }
    }
}

/* Apply a complete pins.ini body and answer. Runs inside the flash window. */
static void apply_api_pins(http_conn_t *hc, char *body) {
    extern flight_state_t flight_get_state(void);
    flight_state_t st = flight_get_state();

    if (st != PAD_IDLE) {
        /* Same interlock as /api/config: a pin map that changes under a flying
         * board would move the pyro pins mid-flight. */
        char jb[160];
        int jn = snprintf(jb, sizeof(jb), "{\"error\":\"Device not ready (state=%s)\",\"reboot_required\":true}",
                          state_names[st < STATE_NAME_COUNT ? st : 0]);
        http_respond(hc, 409, JSON, jb, (uint32_t)jn);
    } else {
        /* Merged over the live assignment for the same reason /api/config
         * merges (CFG-06): a partial post must not silently release a channel
         * by omitting its key.
         *
         * static, not a local: pin_assign_t is about 300 bytes, and nothing
         * else runs this -- the service loop is core0's alone. */
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

/* ── GET /api/pins/caps ───────────────────────────────────────────
 *
 * What this board offers, what is assigned to it now, and the vocabulary to
 * read both in. The browser has never learned anything board-specific beyond
 * the board name, which is why the Lua tab rendered MK1C's four J3 pads on
 * every board and offered roles no pin here can take.
 *
 * The vocabulary travels WITH the data: `fn` names the capability bits and
 * `roles` names each role together with the bit it requires. A UI that reads
 * both filters its menus by the same rule pin_assign_validate() enforces,
 * rather than by a copy of it that drifts. Nothing here needs updating when a
 * bit or a role is added -- only the tables they come from. */
static void serve_api_pin_caps(http_conn_t *hc) {
    /* The connection's work buffer, which HTTP_WORK_SIZE sizes for this, the
     * largest response the server builds. Sized from the worst case rather
     * than from today's boards. A pin row is at most ~110 bytes with every
     * field at its longest -- the connector label added ~25 -- and RP2040 has
     * 30 GPIOs, so the rows can reach ~3300; the fn map, the role list and the
     * protection sentence add ~1000. MK1C already serves 1878 with most names
     * empty. */
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

    int pos = snprintf(buf, cap,
                       "{\"board\":\"%s\",\"topology\":\"%s\",\"bridge_possible\":%s,"
                       "\"protection_note\":\"%s\","
                       "\"pyro1_released\":%s,\"pyro2_released\":%s,\"reserved_mask\":%u,"
                       "\"buzzer_pin\":%d,\"buzzer_on\":%d,\"fn\":{",
                       PYRO_BOARD_NAME, pin_caps_topology_name(), pin_caps_bridge_possible() ? "true" : "false", note,
                       pa->pyro1_released ? "true" : "false", pa->pyro2_released ? "true" : "false",
                       (unsigned)FN_BOARD_RESERVED,
                       /* buzzer_pin is the SETTING (-1 = leave it where the
                        * board put it); buzzer_on is where it actually is, so
                        * the UI can show the default without resolving it. */
                       pa->buzzer_pin == PIN_BUZZER_BOARD ? -1 : (int)pa->buzzer_pin,
                       pin_assign_buzzer_pin(pa) == PIN_BUZZER_BOARD ? -1 : (int)pin_assign_buzzer_pin(pa));

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

    /* Truncated JSON parses as nothing and would leave the tab silently
     * empty, so say so instead. The buffer is sized for the largest board's
     * table with room to spare; reaching here means one outgrew it. */
    if (pos < 0 || pos >= (int)cap) {
        http_respond_str(hc, 500, JSON, "{\"error\":\"capability table exceeds the response buffer\"}");
        return;
    }
    http_respond(hc, 200, JSON, buf, (uint32_t)pos);
}

static void serve_api_status(http_conn_t *hc) {
    char pins_reason_esc[128];
    const char *pr = pin_store_reason();
    json_escape(pins_reason_esc, (int)sizeof(pins_reason_esc), pr, (int)strlen(pr));

    /* The live assignment, which phase 4's Config tab renders and which makes
     * "did my pins.ini actually take effect" answerable without a debugger. */
    const pin_assign_t *pa = pin_store_current();
    char bridge_desc[24];
    uint8_t br_ch, br_common;
    const char *br_name;
    if (pin_store_bridge(&br_ch, &br_common, &br_name)) {
        snprintf(bridge_desc, sizeof(bridge_desc), "%u+%u", (unsigned)br_ch, (unsigned)br_common);
    } else {
        snprintf(bridge_desc, sizeof(bridge_desc), "none");
    }
    /* What is wrong, named, as distinct from what the buzzer says about it.
     *
     * The beep is one of three, because three is the number of actions
     * available at the pad. This is the screen, so it carries the diagnosis:
     * nobody has to count beeps to read it. */
    extern flight_context_t *flight_get_context(void);
    const flight_context_t *fctx = flight_get_context();
    char fault_list[160] = {0};
    if (fctx) {
        int fl = 0;
        for (uint16_t bit = 1; bit != 0; bit <<= 1) {
            if ((fctx->diag & bit) && *flight_diag_name(bit) && fl < (int)sizeof(fault_list) - 24) {
                fl += snprintf(fault_list + fl, sizeof(fault_list) - (size_t)fl, "%s\"%s\"", fl ? "," : "",
                               flight_diag_name(bit));
            }
        }
    }

    /* Which outcome, and how it sounds under the active personality, so it can
     * be read rather than counted. */
    extern beep_reason_t beep_reason_for_diag(uint16_t diag);
    uint16_t diag_now = fctx ? fctx->diag : 0;
    beep_reason_t beep_r = beep_reason_for_diag(diag_now);
    beep_spec_t beep_sp = beep_for(beep_r);
    char beep_sound[16];
    if (beep_sp.kind != BK_CODE) {
        snprintf(beep_sound, sizeof(beep_sound), "%s", beep_codes_kind_name((beep_kind_t)beep_sp.kind));
    } else if (beep_sp.d2 == 0) {
        snprintf(beep_sound, sizeof(beep_sound), "%u", (unsigned)beep_sp.d1);
    } else {
        snprintf(beep_sound, sizeof(beep_sound), "%u-%u", (unsigned)beep_sp.d1, (unsigned)beep_sp.d2);
    }

    /* MK1C carries the most fields -- the bias probes, pack voltage and wave
     * state on top of everything shared -- and at 1280 it had begun truncating
     * mid-word. */
    char *buf = (char *)hc->work;
    const size_t cap = 2048;
    const char *sn = (g_status.state < (int)(sizeof(state_names) / sizeof(state_names[0])))
                         ? state_names[g_status.state]
                         : "UNKNOWN";
    static const char *mode_names[] = {"none", "fallen", "agl", "speed", "delay"};
    const char *p1m = (g_status.pyro1_mode < 5) ? mode_names[g_status.pyro1_mode] : "?";
    const char *p2m = (g_status.pyro2_mode < 5) ? mode_names[g_status.pyro2_mode] : "?";

    /* Raw pyro sense counts; -1 on a board that has none. Reported as raw
     * ADC counts rather than volts so a marginal reading stays visible. */
    board_pyro_raw_t praw = {0};
    int raw_busq = -1, raw_bus = -1, raw_vbat = -1, raw_a = -1, raw_b = -1, raw_tau = -1;
    if (board_pyro_raw(&praw)) {
        raw_busq = (int)praw.bus_quiescent;
        raw_bus = (int)praw.bus_biased;
        raw_vbat = (int)praw.vbat;
        raw_a = (int)praw.ch_a_biased;
        raw_b = (int)praw.ch_b_biased;
        raw_tau = (int)praw.bus_decay_tau_us;
    }
    int pos = snprintf(
        buf, cap,
        "{\"state\":\"%s\",\"alt_cm\":%ld,\"max_alt_cm\":%ld,"
        "\"vspeed_cms\":%ld,\"pressure_pa\":%ld,"
        "\"pyro1_cont\":%s,\"pyro2_cont\":%s,"
        "\"pyro1_adc\":%u,\"pyro2_adc\":%u,"
        "\"pyro1_fired\":%s,\"pyro2_fired\":%s,"
        "\"armed\":%s,\"flight_ms\":%lu,\"uptime\":%lu,\"fw_version\":\"%s\","
        "\"pyro1_mode\":\"%s\",\"pyro1_value\":%u,"
        "\"pyro2_mode\":\"%s\",\"pyro2_value\":%u,"
        "\"units\":%u,\"rocket_id\":\"%.8s\",\"rocket_name\":\"%.8s\","
        "\"sensor\":\"%s\",\"board\":\"%s\","
        "\"pyro_bus_q\":%d,\"pyro_bus_adc\":%d,\"pyro_vbat_adc\":%d,"
        "\"bias_a\":%d,\"bias_b\":%d,\"decay_tau_us\":%d,\"wave_state\":%d,"
        "\"loop_max_us\":%lu,\"loop_overruns\":%lu,\"loop_late_max_us\":%lu,"
        "\"loop_count\":%lu,\"stage_max_us\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],"
        "\"flash_opens\":%lu,\"flash_skips\":%lu,\"flash_refusals\":%lu,\"log_dropped\":%lu,"
        "\"flash_erases\":%lu,\"flash_programs\":%lu,\"flash_deferrals\":%lu,"
        "\"pins_reason\":\"%s\",\"pyro1_released\":%s,\"pyro2_released\":%s,\"bridge\":\"%s\","
        "\"pyro_mocked\":%lu,\"pyro1_real\":%s,\"pyro2_real\":%s,"
        "\"sensor_ok\":%s,\"fs_ok\":%s,\"faults\":[%s],"
        "\"reset_cause\":%u,\"recovery\":\"%s\","
        "\"pyro1_refused\":%s,\"pyro2_refused\":%s,\"pyro1_refires\":%u,\"main_forced\":%s,"
        "\"pres_waits\":%lu,\"pres_rejects\":%lu,\"raw_pa\":%ld,\"pad_speed_cms\":%ld,\"usb_attached\":%s,\"test_"
        "mode\":%s,\"buzzer_active\":%s,"
        "\"beep\":\"%s\",\"beep_sound\":\"%s\","
        "\"serial\":\"%s\",\"serial_assigned\":%s,\"hw_id\":\"%s\",\"subnet\":%u}",
        sn, (long)g_status.altitude_cm, (long)g_status.max_altitude_cm, (long)g_status.vertical_speed_cms,
        (long)g_status.pressure_pa, g_status.pyro1_continuity ? "true" : "false",
        g_status.pyro2_continuity ? "true" : "false", (unsigned)g_status.pyro1_adc, (unsigned)g_status.pyro2_adc,
        g_status.pyro1_fired ? "true" : "false", g_status.pyro2_fired ? "true" : "false",
        g_status.pyros_armed ? "true" : "false", (unsigned long)g_status.flight_time_ms,
        (unsigned long)to_ms_since_boot(get_absolute_time()), FW_VERSION, p1m, (unsigned)g_status.pyro1_value, p2m,
        (unsigned)g_status.pyro2_value, (unsigned)g_status.units, g_status.rocket_id, g_status.rocket_name,
        pressure_sensor_name(), PYRO_BOARD_NAME, raw_busq, raw_bus, raw_vbat, raw_a, raw_b, raw_tau,
        board_pyro_wave_state(), (unsigned long)loop_max_us, (unsigned long)loop_overruns,
        (unsigned long)loop_late_max_us, (unsigned long)loop_count, (unsigned long)stage_max_us[0],
        (unsigned long)stage_max_us[1], (unsigned long)stage_max_us[2], (unsigned long)stage_max_us[3],
        (unsigned long)stage_max_us[4], (unsigned long)stage_max_us[5], (unsigned long)stage_max_us[6],
        (unsigned long)stage_max_us[7], (unsigned long)stage_max_us[8], (unsigned long)flash_window_opens(),
        (unsigned long)flash_window_skips(), (unsigned long)flash_window_refusals(), (unsigned long)hal_log_dropped(),
        (unsigned long)flash_window_erases(), (unsigned long)flash_window_programs(),
        (unsigned long)flash_window_deferrals(), pins_reason_esc, pa->pyro1_released ? "true" : "false",
        pa->pyro2_released ? "true" : "false", bridge_desc, (unsigned long)pyro_release_mocks(),
        /* What the CLAIM decided, next to what the assignment asked for. They
         * agree in every normal case; showing both is how a disagreement --
         * a channel that could not take its pads -- becomes visible rather
         * than being read off the intent. */
        pyro_release_is_released(1) ? "false" : "true", pyro_release_is_released(2) ? "false" : "true",
        /* The power-up self-test, said out loud: a board that cannot measure
         * altitude must not report itself healthy. */
        fctx && fctx->sensor_type ? "true" : "false", fctx && fctx->fs_ok ? "true" : "false", fault_list,
        /* Why this boot happened and what was made of it. A brownout reads as
         * a power event, so the phrase is the part worth reading. */
        fctx ? (unsigned)fctx->reset_cause : 0u,
        brownout_recovery_name(fctx ? (recovery_t)fctx->recovery : RECOVER_COLD),
        /* A deployment the board could not make, and one the ladder made
         * over the operator's trigger: after the flight, these are the
         * difference between a configured main and an emergency one. */
        fctx && fctx->pyro1_refused ? "true" : "false", fctx && fctx->pyro2_refused ? "true" : "false",
        fctx ? (unsigned)fctx->pyro1_refires : 0u, fctx && fctx->main_forced ? "true" : "false",
        (unsigned long)hal_pressure_waits(), (unsigned long)hal_pressure_rejects(),
        /* The newest reading before any filtering, and the speed the launch
         * detector reads: together, the sensor's noise and what it costs. */
        (long)pp_last_raw_pa(), fctx ? (long)fctx->pad_speed_cms : 0L,
        /* A board on USB detects no launch and says nothing, unless it is in
         * test mode [USB-01..03, USB-08]. */
        fctx && fctx->usb_attached ? "true" : "false", fctx && fctx->test_mode ? "true" : "false",
        buzzer_is_active() ? "true" : "false",
        /* What the buzzer says, or would say off USB, and how it sounds, so
         * it can be read rather than counted. */
        beep_codes_key(beep_r), beep_sound, board_serial(), board_serial_assigned() ? "true" : "false", board_hw_id(),
        (unsigned)board_subnet_octet());
    /* Truncated JSON parses as nothing, so "send what fits" showed the web UI
     * a connection failure and told nobody why. Say so instead -- the same
     * rule /api/pins/caps follows. */
    if (pos < 0 || (size_t)pos >= cap) {
        http_respond_str(hc, 500, JSON, "{\"error\":\"status exceeds the response buffer\"}");
        return;
    }
    http_respond(hc, 200, JSON, buf, (uint32_t)pos);
}
/* ── Files ────────────────────────────────────────────────────────── */

#define WWW_HEADERS "Cache-Control: no-store, must-revalidate\r\n"

/* Stream a littlefs file, framed by its size. Read-only, so no flash window:
 * core1 may keep executing from flash while this reads it. The work buffer is
 * the file's cache. Answers fb_status/fb_body when there is no such file. */
static void serve_file(conn_t *c, const char *lfs_path, const char *ctype, const char *extra, uint16_t fb_status,
                       const char *fb_ctype, const char *fb_body) {
    http_conn_t *hc = &c->h;
    c->file_cfg = (struct lfs_file_config){.buffer = hc->work};
    if (lfs_mount(&c->lfs, &lfs_pico_flash_config) == LFS_ERR_OK) {
        c->lfs_mounted = true;
        if (lfs_file_opencfg(&c->lfs, &c->file, lfs_path, LFS_O_RDONLY, &c->file_cfg) == LFS_ERR_OK) {
            c->file_open = true;
            lfs_soff_t size = lfs_file_size(&c->lfs, &c->file);
            if (size >= 0) {
                c->route = R_FILE;
                http_respond_stream(hc, 200, ctype, (uint32_t)size, extra);
                return;
            }
        }
    }
    http_respond_str(hc, fb_status, fb_ctype, fb_body);
}

static uint16_t fill(http_conn_t *hc, uint8_t *dst, uint16_t max) {
    conn_t *c = (conn_t *)hc;
    if (c->route != R_FILE || !c->file_open) {
        return 0;
    }
    lfs_ssize_t n = lfs_file_read(&c->lfs, &c->file, dst, max);
    return n > 0 ? (uint16_t)n : 0;
}

/* ── Default page if /www/index.html missing ──────────────────────── */

static const char DEFAULT_PAGE[] = "<!DOCTYPE html><html><body><h2>" PYRO_BOARD_NAME "</h2>"
                                   "<p>No web files uploaded. POST files to /www/ to set up the UI.</p>"
                                   "<p><a href=\"/api/status\">Status JSON</a></p></body></html>";

static void serve_get(conn_t *c) {
    http_conn_t *hc = &c->h;
    const char *path = hc->path;

    if (strncmp(path, "/api/capture", 12) == 0) {
        /* Bench: queue a high-speed bus capture. The work happens in the main
         * loop; poll wave_state in /api/status until it reads 2, then GET
         * /wave_c.csv or /wave_d.csv. */
        int mode = 0; /* charge */
        if (strstr(path, "m=d")) {
            mode = 1;
        } else if (strstr(path, "m=a")) {
            mode = 2; /* arm: runs the arm element, interlock applies */
        }
        if (board_pyro_wave_request(mode)) {
            http_respond_str(hc, 200, TEXT,
                             (mode == 2)   ? "queued /wave_a.csv"
                             : (mode == 1) ? "queued /wave_d.csv"
                                           : "queued /wave_c.csv");
        } else {
            http_respond_str(hc, 503, TEXT, "refused: busy, unsupported, or arm interlock");
        }
    } else if (strcmp(path, "/api/status") == 0) {
        serve_api_status(hc);
#if PYRO_HAS_LUA
    } else if (strcmp(path, "/api/lua/script") == 0) {
        /* No script yet is a normal state, not an error: the editor should
         * open empty rather than show a 404. */
        serve_file(c, "/" LUA_SCRIPT_PATH, TEXT, NULL, 200, TEXT, "");
    } else if (strcmp(path, "/api/lua/console") == 0) {
        /* Drains core1's console ring and reports its liveness. The heartbeat
         * is what tells the operator core1 is still turning over; a frozen
         * number with a "running" status means the VM is stuck somewhere the
         * instruction hook cannot reach, and core0 will kill it shortly. */
        char text[900];
        int n = lua_app_console_read(text, sizeof(text) - 1);
        text[n] = '\0';
        char esc[1024];
        json_escape(esc, sizeof(esc), text, n);

        /* The status line carries pyro_lua_last_error() verbatim, and a Lua
         * error names its chunk: [string "check"]:128: ... Unescaped, those
         * quotes end the JSON string, so the response stops parsing at
         * exactly the moment it has something to report. */
        char esc_status[192];
        const char *st = lua_app_status();
        json_escape(esc_status, sizeof(esc_status), st, (int)strlen(st));
        /* "running" with a frozen heartbeat and c1_go ahead of c1_seen means
         * core0 handed out a unit core1 never claimed. Without these, that
         * and a VM stuck mid-tick look identical. */
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
        http_respond(hc, 200, JSON, body, (uint32_t)blen);
#endif
    } else if (strcmp(path, "/api/beeps") == 0) {
        serve_api_beeps(hc);
    } else if (strcmp(path, "/api/pins/caps") == 0) {
        serve_api_pin_caps(hc);
    } else if (strcmp(path, "/api/pins") == 0) {
        serve_file(c, "/" PIN_STORE_PATH, TEXT, NULL, 404, TEXT, "No pins.ini");
    } else if (strcmp(path, "/api/config") == 0) {
        serve_file(c, "config.ini", TEXT, NULL, 404, TEXT, "No config.ini");
    } else if (strcmp(path, "/api/flight.csv") == 0) {
        serve_file(c, FLIGHT_LOG_PATH, "text/csv", "Content-Disposition: attachment; filename=\"flight.csv\"\r\n", 200,
                   "text/csv", "time_ms,pressure_pa,altitude_cm,state,thrust,event\r\n");
    } else if (strcmp(path, "/") == 0) {
        /* no-store: the UI is re-uploaded whenever the firmware or web files
         * change, and without this browsers heuristically cache it and keep
         * showing the previous build. */
        serve_file(c, "/www/index.html", "text/html", WWW_HEADERS, 200, "text/html", DEFAULT_PAGE);
    } else {
        serve_file(c, path, content_type_hdr(path), WWW_HEADERS, 404, TEXT, "Not found");
    }
}

/* ── Uploads ──────────────────────────────────────────────────────────
 *
 * Opened on the first body bytes, inside the window, because creating and
 * truncating a file writes its metadata. An upload that dies before its last
 * byte is never closed, so littlefs keeps the previous file whole. */

static bool upload_open(conn_t *c) {
    c->file_cfg = (struct lfs_file_config){.buffer = c->h.work};
    int err = lfs_mount(&c->lfs, &lfs_pico_flash_config);
    if (err != LFS_ERR_OK) {
        DBG("POST %s FAIL lfs_mount err=%d", c->dest, err);
        return false;
    }
    c->lfs_mounted = true;
    if (strncmp(c->dest, "/www/", 5) == 0) {
        lfs_mkdir(&c->lfs, "/www");
    }
    err = lfs_file_opencfg(&c->lfs, &c->file, c->dest, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &c->file_cfg);
    if (err != LFS_ERR_OK) {
        DBG("POST %s FAIL lfs_file_open err=%d", c->dest, err);
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
    if (lfs_file_write(&c->lfs, &c->file, data, len) != (lfs_ssize_t)len) {
        DBG("POST %s FAIL write refused", c->dest);
        http_respond_str(&c->h, 500, TEXT, "write failed; file is incomplete, retry");
    }
    return len;
}

static void upload_complete(conn_t *c) {
    /* lfs_file_close() flushes the last partial block, so a refusal there
     * loses the tail of the file as quietly as a refused write does. */
    bool ok = c->file_open && lfs_file_close(&c->lfs, &c->file) == LFS_ERR_OK;
    c->file_open = false;
    c->file_writing = false;
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
        /* Unreachable inside the window, and a truncated image if that
         * reasoning is wrong. */
        ota_failed = true;
        http_respond_str(&c->h, 500, TEXT, "OTA aborted: flash window closed mid-image");
    }
    return len;
}

/* ── Small POSTs ──────────────────────────────────────────────────── */

/* Provisioning: the board's assigned MAC, into /serial.txt.
 *
 * Exactly 12 characters, validated as hex and as a unicast address before
 * anything touches flash. A board that accepts a bad identity is one that
 * will not enumerate usefully and cannot be reached to fix it. Takes effect at
 * the next boot: the MAC goes into the ECM descriptor and the subnet into the
 * DHCP server, both of which the host reads once, at enumeration. */
static bool serial_valid(const char *s) {
    for (int i = 0; i < 12; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    /* Bit 0 of the first octet is the multicast bit. A NIC that sources
     * frames from a multicast address is not something to debug later. */
    int hi = (s[0] >= '0' && s[0] <= '9') ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
    int lo = (s[1] >= '0' && s[1] <= '9') ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
    return (((hi << 4) | lo) & 0x01) == 0;
}

static void apply_erase(http_conn_t *hc) {
    /* [DAT-06, WEB-UI-04] There is one log slot, and the next launch
     * overwrites it; this is how an operator clears it on purpose. Refused
     * while the log is still being written -- that is the flight in progress,
     * or its tail being flushed after landing. */
    if (hal_log_active()) {
        http_respond_str(hc, 409, JSON, "{\"error\":\"the flight log is still being written\"}");
        return;
    }
    lfs_t lfs;
    int rc = lfs_mount(&lfs, &lfs_pico_flash_config);
    if (rc == LFS_ERR_OK) {
        rc = lfs_remove(&lfs, FLIGHT_LOG_PATH);
        lfs_unmount(&lfs);
    }
    DBG("POST %s rc=%d", FLIGHT_ERASE_PATH, rc);
    if (rc == LFS_ERR_OK || rc == LFS_ERR_NOENT) {
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
    static lua_chk_result_t chk; /* large, and the service loop is core0's alone */
    lua_app_check((const char *)hc->work, (int)hc->gathered, &flight_get_context()->config, &chk);

    /* The script is done with, so the work buffer takes the answer. */
    char *body = (char *)hc->work;
    const int cap = (int)sizeof(hc->work);
    int blen = snprintf(body, (size_t)cap, "{\"green\":%s,\"items\":[", chk.green ? "true" : "false");
    /* detail carries a Lua error verbatim, and a Lua error names its chunk:
     * [string "check"]:128: ... Unescaped, those quotes end the JSON string
     * and the Check button reports "check failed" instead of the error it was
     * asked to show.
     *
     * blen is bounded on every append because snprintf returns what it WOULD
     * have written: letting it run past cap hands the next call a negative
     * size. */
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

/* The POSTs that carry a body. gather is the most a whole-document route
 * takes into the work buffer; 0 streams the body to on_body instead. */
typedef struct {
    const char *path;
    bool prefix;
    route_t route;
    uint32_t gather;
    bool flash;
} post_route_t;

static const post_route_t post_routes[] = {
    {"/api/ota", false, R_OTA, 0, true},
    {"/www/", true, R_UPLOAD, 0, true},
#if PYRO_HAS_LUA
    /* The Lua program rides the same streaming write as a web file. */
    {"/api/lua/script", false, R_UPLOAD, 0, true},
    {"/api/lua/check", false, R_LUA_CHECK, 2047, false},
#endif
    {"/api/serial", false, R_SERIAL, 12, true},
    {"/api/config", false, R_CONFIG, 511, true},
    {"/api/beeps/play", false, R_BEEP_PLAY, 63, false},
    {"/api/beeps", false, R_BEEPS, BEEP_STORE_MAX - 1, true},
    {"/api/pins", false, R_PINS, PIN_STORE_MAX - 1, true},
};

static const post_route_t *find_post_route(const char *path) {
    for (unsigned i = 0; i < sizeof(post_routes) / sizeof(post_routes[0]); i++) {
        const post_route_t *r = &post_routes[i];
        if (r->prefix ? strncmp(path, r->path, strlen(r->path)) == 0 : strcmp(path, r->path) == 0) {
            return r;
        }
    }
    return NULL;
}

static void route_post(conn_t *c) {
    http_conn_t *hc = &c->h;
    const char *path = hc->path;

    if (strcmp(path, TEST_MODE_ON_PATH) == 0 || strcmp(path, TEST_MODE_OFF_PATH) == 0) {
        apply_api_test_mode(hc, strcmp(path, TEST_MODE_ON_PATH) == 0);
        return;
    }
    if (strcmp(path, "/api/reboot") == 0) {
        DBG("POST /api/reboot");
        c->reboot_when_sent = true;
        http_respond_str(hc, 200, TEXT, "Rebooting");
        return;
    }
    if (strcmp(path, FLIGHT_ERASE_PATH) == 0) {
        c->route = R_ERASE;
        flash_ready(c);
        return;
    }

    const post_route_t *r = find_post_route(path);
    if (!r) {
        http_respond_str(hc, 404, TEXT, "Not found");
        return;
    }
    if (hc->content_length == 0) {
        http_respond_str(hc, 400, TEXT, "this request needs a body");
        return;
    }
    if (r->route == R_OTA && ota_conn && ota_conn != c) {
        http_respond_str(hc, 409, TEXT, "another update is in progress");
        return;
    }
    c->route = r->route;
    if (r->gather) {
        http_gather(hc, r->gather);
    } else {
        http_stream(hc);
    }
    if (r->flash) {
        flash_ready(c);
    }
    if (r->route == R_OTA) {
        /* Per transfer. Left latched, a failure would abort every later
         * upload at its first byte. */
        ota_conn = c;
        ota_offset = 0;
        ota_buf_fill = 0;
        ota_failed = false;
        pfb_started = false;
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
    conn_t *c = (conn_t *)hc;
    if (refused_in_flight(hc->method, hc->path)) {
        http_respond_str(hc, 409, JSON, "{\"error\":\"refused while the rocket is in flight\"}");
    } else if (strcmp(hc->method, "POST") == 0) {
        route_post(c);
    } else {
        serve_get(c);
    }
}

static uint16_t on_body(http_conn_t *hc, const uint8_t *data, uint16_t len) {
    conn_t *c = (conn_t *)hc;
    if (!flash_ready(c)) {
        return 0;
    }
    if (c->route == R_OTA) {
        if (!pfb_started) {
            pfb_firmware_commit();
            pfb_started = true;
        }
        return ota_body(c, data, len);
    }
    return upload_body(c, data, len);
}

static bool on_complete(http_conn_t *hc) {
    conn_t *c = (conn_t *)hc;
    char *body = (char *)hc->work;
    switch (c->route) {
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

    if (!flash_ready(c)) {
        return false;
    }
    switch (c->route) {
    case R_UPLOAD:
        upload_complete(c);
        break;
    case R_OTA:
        ota_flush(); /* the window is open: the tail sector cannot be refused */
        pfb_mark_download_slot_as_valid();
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
    case R_SERIAL:
        if (hal_fs_write_file("serial.txt", body, 12) != 0) {
            http_respond_str(hc, 500, TEXT, "write failed");
        } else {
            http_respond_str(hc, 200, TEXT, "OK, reboot to apply");
        }
        break;
    case R_ERASE:
        apply_erase(hc);
        break;
    default:
        http_respond_str(hc, 404, TEXT, "Not found");
        break;
    }
    /* Done with flash: two seconds of parked Lua would buy nothing. */
    release_window(c);
    return true;
}

static const http_handlers_t handlers = {
    .common_headers = CORS_HDR,
    .on_head = on_head,
    .on_body = on_body,
    .on_complete = on_complete,
    .fill = fill,
};

/* ── The lwIP adapter ─────────────────────────────────────────────── */

extern volatile uint32_t net_http_accept;
extern volatile uint32_t net_http_err;
extern volatile uint32_t net_conn_full;

static void conn_release(conn_t *c) {
    if (c->file_open && !c->file_writing) {
        lfs_file_close(&c->lfs, &c->file);
    }
    c->file_open = false;
    c->file_writing = false;
    if (c->lfs_mounted) {
        lfs_unmount(&c->lfs);
        c->lfs_mounted = false;
    }
    release_window(c);
    if (ota_conn == c) {
        ota_conn = NULL;
    }
    if (c->reboot_when_sent) {
        /* The main loop arms a 100 ms watchdog and keeps servicing lwIP, so
         * the reply now in lwIP's hands reaches the client. */
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
    c->lfs_mounted = false;
    c->file_open = false;
    c->file_writing = false;
    c->holding = false;
    c->deferral_counted = false;
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
    *l = (link_t){.pcb = pcb, .seq = ++link_seq, .last_ms = hal_time_ms()};
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

static void service_link(link_t *l, uint32_t now) {
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

    while (l->pending && net_ring_writable(&hc->rx) > 0) {
        uint8_t *p;
        uint16_t span = net_ring_write_span(&hc->rx, &p);
        uint16_t n = pbuf_copy_partial(l->pending, p, span, 0);
        net_ring_commit(&hc->rx, n);
        l->pending = pbuf_free_header(l->pending, n);
    }
    hc->rx_eof = l->fin && !l->pending;

    http_conn_service(hc, &handlers);

    /* The window reopens by what the parser took, not by what arrived:
     * a request waiting on the flash window holds its sender back. */
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
        if (n == 0 || tcp_write(pcb, p, n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
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
        link_abort(l);
    }
}

/* From the main loop, after lwIP's own input and timers: net_service(). */
void http_server_service(void) {
    uint32_t now = hal_time_ms();
    attach_waiting();
    for (int i = 0; i < LINK_POOL_SIZE; i++) {
        if (links[i].pcb) {
            service_link(&links[i], now);
        }
    }
}

void http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 80);
    pcb = tcp_listen_with_backlog(pcb, 8);
    tcp_accept(pcb, on_accept);
}
