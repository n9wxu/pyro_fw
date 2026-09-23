/*
 * Streaming HTTP server: serves files from littlefs /www/, API endpoints.
 * Handles any file size via chunked read/write with per-connection state.
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

/* Defined by src/lua/lua_core1.c; weak no-ops in littlefs_driver.c when
 * Lua is not linked. */
#if PYRO_HAS_LUA
#include "lua_app.h"
#include "lua_core1.h"
#endif

#include "flash_window.h"
#include "pin_store.h"

extern uint32_t hal_time_ms(void);

extern void hal_telemetry_send(const char *sentence);
#define DBG(fmt, ...)                                                                                                  \
    do {                                                                                                               \
        char _b[128];                                                                                                  \
        snprintf(_b, sizeof(_b), "HTTP: " fmt "\r\n", ##__VA_ARGS__);                                                  \
        hal_telemetry_send(_b);                                                                                        \
    } while (0)

extern const char *pressure_sensor_name(void);

#define CORS_HDR "Access-Control-Allow-Origin: *\r\n"

extern const struct lfs_config lfs_pico_flash_config;
extern const struct lfs_file_config lfs_pico_file_config;

#define CHUNK_SIZE 512

/* ── Per-connection state ─────────────────────────────────────────── */

typedef enum {
    CONN_IDLE,
    CONN_SENDING_FILE,
    CONN_RECEIVING_FILE,
    CONN_RECEIVING_OTA,
} conn_phase_t;

typedef struct {
    conn_phase_t phase;
    lfs_t lfs;
    lfs_file_t file;
    bool lfs_mounted;
    bool file_open;
    uint32_t remaining; /* bytes left to receive */
    bool write_failed;  /* an lfs write was refused: the file is a hole */
    char path[64];
    /* Under LFS_NO_MALLOC the caller owns the per-file cache. It cannot be
     * one shared buffer: up to CONN_POOL_SIZE connections can hold a file
     * open at once, and they would corrupt each other's cache. One per
     * connection is the correct granularity -- the same granularity littlefs
     * would have malloc'd at, just statically and without the cross-core
     * mutex that makes malloc unsafe here (docs/core1_hazard.md). */
    uint8_t file_buf[FLASH_SECTOR_SIZE];
    struct lfs_file_config file_cfg;
} conn_state_t;

#define CONN_POOL_SIZE 4

static conn_state_t conn_pool[CONN_POOL_SIZE];

static conn_state_t *conn_alloc(void) {
    for (int i = 0; i < CONN_POOL_SIZE; i++)
        if (conn_pool[i].phase == CONN_IDLE) {
            /* Clear the bookkeeping but not the 4 kB cache: memset of the
             * whole struct would now cost a sector-sized wipe per accept. */
            conn_state_t *cs = &conn_pool[i];
            cs->phase = CONN_IDLE;
            cs->lfs_mounted = false;
            cs->file_open = false;
            cs->remaining = 0;
            cs->write_failed = false;
            cs->path[0] = '\0';
            cs->file_cfg.buffer = cs->file_buf;
            return cs;
        }
    return NULL;
}

/* lfs_file_close() flushes the last partial block, so a refusal there loses
 * the tail of the file as quietly as a refused write does. */
static bool conn_free(conn_state_t *cs) {
    bool ok = true;
    if (cs->file_open) {
        if (lfs_file_close(&cs->lfs, &cs->file) != LFS_ERR_OK) {
            ok = false;
        }
        cs->file_open = false;
    }
    if (cs->lfs_mounted) {
        lfs_unmount(&cs->lfs);
        cs->lfs_mounted = false;
    }
    cs->phase = CONN_IDLE;
    return ok;
}

/* ── OTA firmware update state ────────────────────────────────────── */

/* Download slot flash offset (from linker symbols) */
extern uint32_t __FLASH_DOWNLOAD_SLOT_START;
#define OTA_SLOT_OFF ((uint32_t) & __FLASH_DOWNLOAD_SLOT_START - XIP_BASE)

static uint8_t ota_buf[FLASH_SECTOR_SIZE] __attribute__((aligned(FLASH_PAGE_SIZE)));
static uint32_t ota_offset; /* bytes written so far */
static uint16_t ota_buf_fill;
static bool ota_failed;

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

/* Only these need scheduling into core0's flash window; every other request
 * answers from RAM whatever core1 is doing. */
static bool post_writes_flash(const char *path) {
    return strcmp(path, "/api/ota") == 0 || strcmp(path, "/api/config") == 0 || strcmp(path, "/api/serial") == 0 ||
           strcmp(path, "/api/lua/script") == 0 || strcmp(path, "/api/pins") == 0 || strncmp(path, "/www/", 5) == 0;
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

/* ── Send next chunk of file (called from sent callback) ──────────── */

static void send_next_chunk(struct tcp_pcb *pcb, conn_state_t *cs) {
    char buf[CHUNK_SIZE];
    /* Write as many chunks as the send buffer can hold */
    while (tcp_sndbuf(pcb) >= CHUNK_SIZE) {
        lfs_ssize_t n = lfs_file_read(&cs->lfs, &cs->file, buf, CHUNK_SIZE);
        if (n <= 0) {
            /* EOF — flush and close after send completes */
            tcp_output(pcb);
            conn_free(cs);
            tcp_arg(pcb, NULL);
            tcp_close(pcb);
            return;
        }
        err_t err = tcp_write(pcb, buf, n, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            lfs_file_seek(&cs->lfs, &cs->file, -n, LFS_SEEK_CUR);
            break;
        }
    }
    tcp_output(pcb);
}

/* Forward declaration — on_sent is defined after the API handlers but
 * serve_lfs_file_streaming() needs it to set up the sent callback. */
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len);

/* ── API handlers ─────────────────────────────────────────────────── */

static const char *state_names[] = {"BOOT_SETTLE", "BOOT_CONTINUITY", "BOOT_CALIBRATE", "PAD_IDLE", "ASCENT",
                                    "FALLING",     "DROGUE_DESCENT",  "CHUTE_DESCENT",  "LANDED"};

/* Main-loop pacing counters (main_hardware.c). Reported so the budget a
 * board declares in board.cmake can be checked against what it actually
 * does, rather than being taken on trust. */
extern volatile uint32_t loop_count, loop_max_us, loop_overruns, loop_late_max_us;
extern volatile uint32_t stage_max_us[];

#include "board_identity.h"

static void serve_api_status(struct tcp_pcb *pcb) {
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
    char buf[1280];
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
        buf, sizeof(buf),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" CORS_HDR "Connection: close\r\n\r\n"
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
        pa->pyro2_released ? "true" : "false", bridge_desc, board_serial(),
        board_serial_assigned() ? "true" : "false", board_hw_id(), (unsigned)board_subnet_octet());
    if (pos < 0)
        return;
    if ((size_t)pos >= sizeof(buf))
        pos = (int)sizeof(buf) - 1; /* truncated: send what fits, never past it */
    tcp_write(pcb, buf, pos, TCP_WRITE_FLAG_COPY);
}

/* Streaming API file serve — uses conn_state_t so the main loop keeps
 * running between chunks (no blocking read-all-then-send pattern).
 * Returns the allocated conn_state_t, or NULL if it fell back to a
 * one-shot response (error / not-found). */
static conn_state_t *serve_lfs_file_streaming(struct tcp_pcb *pcb, const char *lfs_path, const char *http_hdr,
                                              const char *fallback_resp) {
    conn_state_t *cs = conn_alloc();
    if (!cs) {
        tcp_write(pcb, fallback_resp, strlen(fallback_resp), TCP_WRITE_FLAG_COPY);
        return NULL;
    }
    if (lfs_mount(&cs->lfs, &lfs_pico_flash_config) != LFS_ERR_OK) {
        conn_free(cs);
        tcp_write(pcb, fallback_resp, strlen(fallback_resp), TCP_WRITE_FLAG_COPY);
        return NULL;
    }
    cs->lfs_mounted = true;
    if (lfs_file_opencfg(&cs->lfs, &cs->file, lfs_path, LFS_O_RDONLY, &cs->file_cfg) != LFS_ERR_OK) {
        conn_free(cs);
        tcp_write(pcb, fallback_resp, strlen(fallback_resp), TCP_WRITE_FLAG_COPY);
        return NULL;
    }
    cs->file_open = true;
    cs->phase = CONN_SENDING_FILE;
    tcp_arg(pcb, cs);
    tcp_write(pcb, http_hdr, strlen(http_hdr), TCP_WRITE_FLAG_COPY);
    send_next_chunk(pcb, cs);
    tcp_sent(pcb, on_sent);
    return cs;
}

static conn_state_t *serve_api_config(struct tcp_pcb *pcb) {
    return serve_lfs_file_streaming(
        pcb, "config.ini", "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" CORS_HDR "Connection: close\r\n\r\n",
        "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNo config.ini");
}

static conn_state_t *serve_api_flight_csv(struct tcp_pcb *pcb) {
    return serve_lfs_file_streaming(pcb, "flight_log.csv",
                                    "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\n"
                                    "Content-Disposition: attachment; filename=\"flight.csv\"\r\n" CORS_HDR
                                    "Connection: close\r\n\r\n",
                                    "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\n" CORS_HDR "Connection: close\r\n\r\n"
                                    "time_ms,pressure_pa,altitude_cm,state,thrust,event\r\n");
}

/* ── Default page if /www/index.html missing ──────────────────────── */

static const char *DEFAULT_PAGE = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
                                  "<!DOCTYPE html><html><body><h2>" PYRO_BOARD_NAME "</h2>"
                                  "<p>No web files uploaded. POST files to /www/ to set up the UI.</p>"
                                  "<p><a href=\"/api/status\">Status JSON</a></p></body></html>";

/* ── TCP callbacks ────────────────────────────────────────────────── */

static void on_err(void *arg, err_t err) {
    (void)err;
    conn_state_t *cs = (conn_state_t *)arg;
    if (cs)
        conn_free(cs);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)len;
    conn_state_t *cs = (conn_state_t *)arg;
    if (cs && cs->phase == CONN_SENDING_FILE) {
        send_next_chunk(pcb, cs); /* may close connection on EOF */
    } else if (!cs) {
        tcp_close(pcb); /* non-file response fully sent */
    } else {
        conn_free(cs);
        tcp_close(pcb);
    }
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err;
    conn_state_t *cs = (conn_state_t *)arg;

    if (!p) {
        if (cs)
            conn_free(cs);
        tcp_close(pcb);
        return ERR_OK;
    }

    /* ── Receiving file upload (continuation packets) ──────────── */
    if (cs && cs->phase == CONN_RECEIVING_FILE) {
        /* All-or-nothing per segment: consuming half and then refusing makes
         * lwIP redeliver the whole segment, duplicating what was written. */
        flash_window_hold(hal_time_ms());
        if (!flash_window_is_open()) {
            flash_window_deferred();
            return ERR_MEM;
        }
        char buf[CHUNK_SIZE];
        uint16_t off = 0;
        while (off < p->tot_len && cs->remaining > 0) {
            uint16_t chunk = p->tot_len - off;
            if (chunk > CHUNK_SIZE)
                chunk = CHUNK_SIZE;
            if (chunk > cs->remaining)
                chunk = cs->remaining;
            pbuf_copy_partial(p, buf, chunk, off);
            if (lfs_file_write(&cs->lfs, &cs->file, buf, chunk) != (lfs_ssize_t)chunk) {
                cs->write_failed = true;
            }
            off += chunk;
            cs->remaining -= chunk;
        }
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);

        if (cs->write_failed) {
            /* Check every write: an unchecked refusal leaves a file of the
             * right length that is wrong in the middle, with nothing
             * reported. */
            DBG("POST %s FAIL write refused", cs->path);
            conn_free(cs);
            flash_window_release();
            const char *fail = "HTTP/1.1 500 Internal Server Error\r\n" CORS_HDR
                               "Connection: close\r\n\r\nwrite failed; file is incomplete, retry";
            tcp_write(pcb, fail, strlen(fail), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
            return ERR_OK;
        }

        if (cs->remaining == 0) {
            DBG("POST %s done (multi pkt)", cs->path);
            if (!conn_free(cs)) {
                flash_window_release();
                const char *fail = "HTTP/1.1 500 Internal Server Error\r\n" CORS_HDR
                                   "Connection: close\r\n\r\nclose failed; file is incomplete, retry";
                tcp_write(pcb, fail, strlen(fail), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_sent(pcb, on_sent);
                tcp_arg(pcb, NULL);
                return ERR_OK;
            }
            flash_window_release();
            const char *resp = "HTTP/1.1 201 Created\r\nConnection: close\r\n\r\nOK";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
        }
        return ERR_OK;
    }

    /* ── Receiving OTA firmware (continuation packets) ─────────── */
    if (cs && cs->phase == CONN_RECEIVING_OTA) {
        flash_window_hold(hal_time_ms());
        if (!flash_window_is_open()) {
            flash_window_deferred();
            return ERR_MEM;
        }
        char buf[CHUNK_SIZE];
        uint16_t off = 0;
        while (off < p->tot_len && cs->remaining > 0) {
            uint16_t chunk = p->tot_len - off;
            if (chunk > CHUNK_SIZE)
                chunk = CHUNK_SIZE;
            if (chunk > cs->remaining)
                chunk = cs->remaining;
            pbuf_copy_partial(p, buf, chunk, off);
            if (ota_write(buf, chunk) != chunk) {
                /* Unreachable behind the gate above, since only core0's
                 * exec loop closes the window and cannot while this callback
                 * runs. Handled anyway: the alternative to noticing is a
                 * truncated image. */
                ota_failed = true;
            }
            off += chunk;
            cs->remaining -= chunk;
        }
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);

        if (ota_failed) {
            conn_free(cs);
            flash_window_release();
            const char *fail = "HTTP/1.1 500 Internal Server Error\r\n" CORS_HDR
                               "Connection: close\r\n\r\nOTA aborted: flash window closed mid-image";
            tcp_write(pcb, fail, strlen(fail), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
            return ERR_OK;
        }

        if (cs->remaining == 0) {
            ota_flush(); /* window is open: the tail sector cannot be refused */
            conn_free(cs);
            flash_window_release();
            pfb_mark_download_slot_as_valid();
            const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOTA OK, rebooting...";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            pfb_perform_update(); /* reboots via watchdog */
        }
        return ERR_OK;
    }

    /* ── First packet: parse request line ──────────────────────── */
    char hdr[512] = {0};
    uint16_t hdr_len = p->tot_len < sizeof(hdr) - 1 ? p->tot_len : sizeof(hdr) - 1;
    pbuf_copy_partial(p, hdr, hdr_len, 0);

    /* Parse method and path */
    char method[8] = {0}, path[64] = {0};
    char *sp1 = memchr(hdr, ' ', hdr_len);
    if (sp1) {
        int mlen = sp1 - hdr;
        if (mlen > 7)
            mlen = 7;
        memcpy(method, hdr, mlen);
        sp1++;
        char *sp2 = memchr(sp1, ' ', hdr_len - (sp1 - hdr));
        if (sp2) {
            int plen = sp2 - sp1;
            if (plen > 63)
                plen = 63;
            memcpy(path, sp1, plen);
        }
    }

    /* Parse Content-Length */
    uint32_t content_length = 0;
    const char *cl = strstr(hdr, "Content-Length: ");
    if (!cl)
        cl = strstr(hdr, "content-length: ");
    if (cl)
        content_length = atoi(cl + 16);

    /* Find body start */
    const char *body_start = strstr(hdr, "\r\n\r\n");
    uint16_t body_offset = body_start ? (body_start + 4 - hdr) : p->tot_len;
    uint16_t body_in_first = (p->tot_len > body_offset) ? p->tot_len - body_offset : 0;

    /* ── The flash gate, before the ack ────────────────────────
     *
     * Return before tcp_recved() and before pbuf_free(). lwIP then keeps the
     * segment as pcb->refused_data and redelivers it a period later (tcp_in.c:
     * "keep incoming packet, because pcb is full"), so nothing is parsed
     * twice, no connection slot is allocated and freed, and no byte is
     * lost. */
    if (strcmp(method, "POST") == 0 && post_writes_flash(path) && content_length > 0) {
        flash_window_hold(hal_time_ms());
        if (!flash_window_is_open()) {
            flash_window_deferred();
            return ERR_MEM;
        }
    }

    tcp_recved(pcb, p->tot_len);

    /* ── Route: GET ────────────────────────────────────────────── */
    if (strcmp(method, "GET") == 0) {
        pbuf_free(p);

        if (strncmp(path, "/api/capture", 12) == 0) {
            /* Bench: queue a high-speed bus capture. The work happens in the
             * main loop; poll wave_state in /api/status until it reads 2,
             * then GET /wave_c.csv or /wave_d.csv. */
            int mode = 0; /* charge */
            if (strstr(path, "m=d"))
                mode = 1;
            else if (strstr(path, "m=a"))
                mode = 2; /* arm: runs the arm element, interlock applies */
            const char *resp;
            if (board_pyro_wave_request(mode))
                resp = (mode == 2)   ? "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n\r\nqueued /wave_a.csv"
                       : (mode == 1) ? "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n\r\nqueued /wave_d.csv"
                                     : "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n\r\nqueued /wave_c.csv";
            else
                resp = "HTTP/1.1 503 Service Unavailable\r\n" CORS_HDR
                       "Connection: close\r\n\r\nrefused: busy, unsupported, or arm interlock";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
            return ERR_OK;
        }

        if (strcmp(path, "/api/status") == 0) {
            serve_api_status(pcb);
#if PYRO_HAS_LUA
        } else if (strcmp(path, "/api/lua/script") == 0) {
            /* No script yet is a normal state, not an error: the editor
             * should open empty rather than show a 404. Both strings must
             * end the header block, or the client waits for headers that
             * never come. */
            cs = serve_lfs_file_streaming(pcb, "/" LUA_SCRIPT_PATH,
                                          "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n"
                                          "Content-Type: text/plain\r\n\r\n",
                                          "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n"
                                          "Content-Type: text/plain\r\n\r\n");
        } else if (strcmp(path, "/api/lua/console") == 0) {
            /* Drains core1's console ring and reports its liveness. The
             * heartbeat is what tells the operator core1 is still turning
             * over; a frozen number with a "running" status means the VM is
             * stuck somewhere the instruction hook cannot reach, and core0
             * will kill it shortly. */
            static char body[1300];
            char text[900];
            int n = lua_app_console_read(text, sizeof(text) - 1);
            text[n] = '\0';
            char esc[1024];
            json_escape(esc, sizeof(esc), text, n);

            /* The status line carries pyro_lua_last_error() verbatim, and a
             * Lua error names its chunk: [string "check"]:128: ... Unescaped,
             * those quotes end the JSON string, so the response stops parsing
             * at exactly the moment it has something to report. */
            char esc_status[192];
            const char *st = lua_app_status();
            json_escape(esc_status, sizeof(esc_status), st, (int)strlen(st));
            /* "running" with a frozen heartbeat and c1_go ahead of c1_seen
             * means core0 handed out a unit core1 never claimed. Without
             * these, that and a VM stuck mid-tick look identical. */
            uint32_t dbg_go, dbg_seen, dbg_skipped, dbg_hb;
            lua_core1_dispatch_stats(&dbg_go, &dbg_seen, &dbg_skipped, &dbg_hb);
            uint32_t dbg_loc = lua_core1_loc();
            int blen =
                snprintf(body, sizeof(body),
                         "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n"
                         "Content-Type: application/json\r\n\r\n"
                         "{\"status\":\"%s\",\"heartbeat\":%lu,\"log_written\":%lu,"
                         "\"console_dropped\":%lu,\"log_dropped\":%lu,\"log_refused\":%lu,"
                         "\"log_active\":%s,"
                         "\"c1_state\":%d,\"c1_loc\":%lu,\"c1_busy\":%lu,\"c1_go\":%lu,\"c1_seen\":%lu,"
                         "\"c1_skipped\":%lu,\"c1_ready\":%s,\"c1_flash_ok\":%s,\"stack_free\":%lu,"
                         "\"text\":\"%s\"}",
                         esc_status, (unsigned long)lua_core1_heartbeat(), (unsigned long)lua_app_log_written(),
                         (unsigned long)lua_core1_console_dropped(), (unsigned long)lua_core1_log_dropped(),
                         (unsigned long)hal_log_text_dropped(), hal_log_active() ? "true" : "false",
                         (int)lua_core1_state(), (unsigned long)(dbg_loc & 0xffu),
                         (unsigned long)((dbg_loc >> 8) & 0xffu), (unsigned long)dbg_go, (unsigned long)dbg_seen,
                         (unsigned long)dbg_skipped, lua_core1_ready() ? "true" : "false",
                         lua_core1_flash_ok() ? "true" : "false", (unsigned long)lua_core1_stack_free(), esc);
            tcp_write(pcb, body, blen, TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
#endif
        } else if (strcmp(path, "/api/pins") == 0) {
            cs = serve_lfs_file_streaming(pcb, "/" PIN_STORE_PATH,
                                          "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n"
                                          "Content-Type: text/plain\r\n\r\n",
                                          "No pins.ini");
        } else if (strcmp(path, "/api/config") == 0) {
            cs = serve_api_config(pcb);
        } else if (strcmp(path, "/api/flight.csv") == 0) {
            cs = serve_api_flight_csv(pcb);
        } else {
            /* Serve file from /www/ */
            const char *fpath = path;
            if (strcmp(path, "/") == 0)
                fpath = "/www/index.html";

            cs = conn_alloc();
            if (!cs) {
                tcp_close(pcb);
                return ERR_OK;
            }

            if (lfs_mount(&cs->lfs, &lfs_pico_flash_config) != LFS_ERR_OK) {
                conn_free(cs);
                tcp_write(pcb, DEFAULT_PAGE, strlen(DEFAULT_PAGE), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_sent(pcb, on_sent);
                tcp_arg(pcb, NULL);
                return ERR_OK;
            }
            cs->lfs_mounted = true;

            if (lfs_file_opencfg(&cs->lfs, &cs->file, fpath, LFS_O_RDONLY, &cs->file_cfg) != LFS_ERR_OK) {
                conn_free(cs);
                if (strcmp(path, "/") == 0) {
                    tcp_write(pcb, DEFAULT_PAGE, strlen(DEFAULT_PAGE), TCP_WRITE_FLAG_COPY);
                } else {
                    const char *r404 = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot found";
                    tcp_write(pcb, r404, strlen(r404), TCP_WRITE_FLAG_COPY);
                }
                tcp_output(pcb);
                tcp_sent(pcb, on_sent);
                tcp_arg(pcb, NULL);
                return ERR_OK;
            }
            cs->file_open = true;
            cs->phase = CONN_SENDING_FILE;
            tcp_arg(pcb, cs);

            /* Send header */
            char resp_hdr[128];
            int hlen =
                /* no-store: the UI is re-uploaded whenever the firmware or web files
                 * change, and without this browsers heuristically cache it and keep
                 * showing the previous build. The whole UI is ~29KB over USB, so
                 * revalidating every load costs nothing. */
                snprintf(resp_hdr, sizeof(resp_hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                         "Cache-Control: no-store, must-revalidate\r\nConnection: close\r\n\r\n",
                         content_type_hdr(fpath));
            tcp_write(pcb, resp_hdr, hlen, TCP_WRITE_FLAG_COPY);

            /* Send first chunk */
            send_next_chunk(pcb, cs);
            tcp_sent(pcb, on_sent);
        }

        if (!cs || cs->phase != CONN_SENDING_FILE) {
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
        }
        return ERR_OK;
    }

    /* ── Route: POST ───────────────────────────────────────────── */
    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/ota") == 0 && content_length > 0) {
            /* ── OTA firmware upload via A/B bootloader ────────── */
            cs = conn_alloc();
            if (!cs) {
                pbuf_free(p);
                tcp_close(pcb);
                return ERR_OK;
            }

            cs->phase = CONN_RECEIVING_OTA;
            cs->remaining = content_length;
            tcp_arg(pcb, cs);

            pfb_firmware_commit();
            ota_offset = 0;
            ota_buf_fill = 0;
            /* Per transfer, like the two above. Left latched, it would abort
             * every later upload at its first continuation packet, reporting
             * a window that is wide open, with only BOOTSEL to clear it. */
            ota_failed = false;

            if (body_in_first > 0) {
                char buf[CHUNK_SIZE];
                uint16_t off = body_offset;
                while (off < p->tot_len && cs->remaining > 0) {
                    uint16_t chunk = p->tot_len - off;
                    if (chunk > CHUNK_SIZE)
                        chunk = CHUNK_SIZE;
                    if (chunk > cs->remaining)
                        chunk = cs->remaining;
                    pbuf_copy_partial(p, buf, chunk, off);
                    /* Unreachable behind the gate, and a truncated image if
                     * that reasoning is wrong. */
                    if (ota_write(buf, chunk) != chunk)
                        ota_failed = true;
                    off += chunk;
                    cs->remaining -= chunk;
                }
            }
            pbuf_free(p);

            /* The body is already consumed, so no later packet can carry the
             * failure and the client would wait on a finished transfer. */
            if (ota_failed) {
                conn_free(cs);
                flash_window_release();
                const char *fail = "HTTP/1.1 500 Internal Server Error\r\n" CORS_HDR
                                   "Connection: close\r\n\r\nOTA aborted: flash window closed mid-image";
                tcp_write(pcb, fail, strlen(fail), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_sent(pcb, on_sent);
                tcp_arg(pcb, NULL);
                return ERR_OK;
            }

            if (cs->remaining == 0) {
                ota_flush();
                conn_free(cs);
                pfb_mark_download_slot_as_valid();
                const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOTA OK, rebooting...";
                tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_sent(pcb, on_sent);
                tcp_arg(pcb, NULL);
                pfb_perform_update();
            }
            return ERR_OK;

#if PYRO_HAS_LUA
        } else if ((strncmp(path, "/www/", 5) == 0 || strcmp(path, "/api/lua/script") == 0) && content_length > 0) {
            /* The Lua program rides the same streaming write as a web file: a
             * script can be several kB, which is more than one TCP segment,
             * and this path already handles that correctly. Only the
             * destination differs. */
            if (strcmp(path, "/api/lua/script") == 0) {
                snprintf(path, sizeof(path), "/%s", LUA_SCRIPT_PATH);
            }

            /* No 503 needed: this path writes littlefs directly rather than
             * through hal_fs_*, and the gate before the ack covers it. */
#else
        } else if (strncmp(path, "/www/", 5) == 0 && content_length > 0) {
#endif
            DBG("POST %s cl=%lu body_in_first=%u", path, (unsigned long)content_length, (unsigned)body_in_first);
            cs = conn_alloc();
            if (!cs) {
                DBG("POST %s FAIL conn_alloc (pool full)", path);
                pbuf_free(p);
                tcp_close(pcb);
                return ERR_OK;
            }

            int mnt_err = lfs_mount(&cs->lfs, &lfs_pico_flash_config);
            if (mnt_err != LFS_ERR_OK) {
                DBG("POST %s FAIL lfs_mount err=%d", path, mnt_err);
                conn_free(cs);
                pbuf_free(p);
                tcp_close(pcb);
                return ERR_OK;
            }
            cs->lfs_mounted = true;
            if (strncmp(path, "/www/", 5) == 0)
                lfs_mkdir(&cs->lfs, "/www");

            int open_err =
                lfs_file_opencfg(&cs->lfs, &cs->file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &cs->file_cfg);
            if (open_err != LFS_ERR_OK) {
                DBG("POST %s FAIL lfs_file_open err=%d", path, open_err);
                conn_free(cs);
                pbuf_free(p);
                tcp_close(pcb);
                return ERR_OK;
            }
            cs->file_open = true;
            cs->phase = CONN_RECEIVING_FILE;
            cs->remaining = content_length;
            strncpy(cs->path, path, sizeof(cs->path) - 1);
            cs->path[sizeof(cs->path) - 1] = '\0';
            tcp_arg(pcb, cs);
            DBG("POST %s open ok remaining=%lu", path, (unsigned long)cs->remaining);

            /* Write body data from first packet */
            if (body_in_first > 0) {
                char buf[CHUNK_SIZE];
                uint16_t off = body_offset;
                while (off < p->tot_len && cs->remaining > 0) {
                    uint16_t chunk = p->tot_len - off;
                    if (chunk > CHUNK_SIZE)
                        chunk = CHUNK_SIZE;
                    if (chunk > cs->remaining)
                        chunk = cs->remaining;
                    pbuf_copy_partial(p, buf, chunk, off);
                    if (lfs_file_write(&cs->lfs, &cs->file, buf, chunk) != (lfs_ssize_t)chunk) {
                        cs->write_failed = true;
                    }
                    off += chunk;
                    cs->remaining -= chunk;
                }
            }
            pbuf_free(p);

            if (cs->remaining == 0 || cs->write_failed) {
                bool ok = !cs->write_failed;
                DBG("POST %s done (single pkt) ok=%d", path, (int)ok);
                if (!conn_free(cs)) {
                    ok = false;
                }
                /* A small file fits in one packet, so two seconds of parked
                 * Lua buys nothing. */
                flash_window_release();
                const char *resp = ok ? "HTTP/1.1 201 Created\r\nConnection: close\r\n\r\nOK"
                                      : "HTTP/1.1 500 Internal Server Error\r\n" CORS_HDR
                                        "Connection: close\r\n\r\nwrite failed; file is incomplete, retry";
                tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_sent(pcb, on_sent);
                tcp_arg(pcb, NULL);
            }
            return ERR_OK;

#if PYRO_HAS_LUA
        } else if (strcmp(path, "/api/lua/check") == 0 && content_length > 0 && content_length < 2048) {
            /* Validate a script against the LIVE resource set without running
             * a line of it. Bounded at one packet on purpose: this is the
             * editor's as-you-go check, and the authoritative one runs on the
             * stored file at boot. */
            static char src[2048];
            uint16_t len = (body_in_first < content_length) ? body_in_first : (uint16_t)content_length;
            pbuf_copy_partial(p, src, len, body_offset);
            pbuf_free(p);
            src[len] = '\0';

            lua_chk_result_t chk;
            extern flight_context_t *flight_get_context(void);
            lua_app_check(src, len, &flight_get_context()->config, &chk);
            static char body[1400];
            int blen = snprintf(body, sizeof(body),
                                "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n"
                                "Content-Type: application/json\r\n\r\n"
                                "{\"green\":%s,\"items\":[",
                                chk.green ? "true" : "false");
            /* detail carries a Lua error verbatim, and a Lua error names its
             * chunk: [string "check"]:128: ... Unescaped, those quotes end the
             * JSON string and the Check button reports "check failed" instead
             * of the error it was asked to show.
             *
             * blen is bounded on every append because snprintf returns what it
             * WOULD have written: letting it run past sizeof(body) hands the
             * next call a negative size. */
            for (int i = 0; i < chk.count && blen > 0 && blen < (int)sizeof(body) - 2; i++) {
                char esc_detail[sizeof(chk.items[i].detail) * 2 + 8];
                json_escape(esc_detail, (int)sizeof(esc_detail), chk.items[i].detail,
                            (int)strlen(chk.items[i].detail));
                int n = snprintf(body + blen, sizeof(body) - (size_t)blen, "%s{\"kind\":%d,\"detail\":\"%s\"}",
                                 i ? "," : "", (int)chk.items[i].kind, esc_detail);
                if (n < 0 || n >= (int)sizeof(body) - blen)
                    break;
                blen += n;
            }
            if (blen > 0 && blen < (int)sizeof(body) - 3) {
                blen += snprintf(body + blen, sizeof(body) - (size_t)blen, "]}");
            }
            tcp_write(pcb, body, blen, TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
            return ERR_OK;
#endif
        } else if (strcmp(path, "/api/serial") == 0 && content_length == 12) {
            /* Provisioning: write the board's assigned MAC to /serial.txt.
             *
             * Exactly 12 characters, validated here as hex and as a unicast,
             * locally-administered address before anything touches flash. A
             * board that accepts a bad identity is one that will not enumerate
             * usefully and cannot be reached to fix it -- so this refuses
             * rather than writes and hopes.
             *
             * Takes effect at the next boot: the MAC goes into the ECM
             * descriptor and the subnet into the DHCP server, both of which
             * the host reads once, at enumeration. */
            char sbuf[16];
            uint16_t len = (body_in_first < content_length) ? body_in_first : content_length;
            pbuf_copy_partial(p, sbuf, len, body_offset);
            pbuf_free(p);
            sbuf[len] = '\0';

            bool ok = (len == 12);
            for (int i = 0; ok && i < 12; i++) {
                char c = sbuf[i];
                ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
            }
            if (ok) {
                /* Bit 0 of the first octet is the multicast bit. A NIC that
                 * sources frames from a multicast address is not something to
                 * debug later. */
                int hi = (sbuf[0] >= '0' && sbuf[0] <= '9') ? sbuf[0] - '0' : (sbuf[0] | 32) - 'a' + 10;
                int lo = (sbuf[1] >= '0' && sbuf[1] <= '9') ? sbuf[1] - '0' : (sbuf[1] | 32) - 'a' + 10;
                ok = ((((hi << 4) | lo) & 0x01) == 0);
            }

            const char *resp;
            if (!ok) {
                resp = "HTTP/1.1 400 Bad Request\r\n" CORS_HDR
                       "Connection: close\r\n\r\nexpected 12 hex digits, unicast (first octet even)";
            } else if (hal_fs_write_file("serial.txt", sbuf, 12) != 0) {
                resp = "HTTP/1.1 500 Internal Server Error\r\n" CORS_HDR "Connection: close\r\n\r\nwrite failed";
            } else {
                resp = "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n\r\nOK, reboot to apply";
            }
            /* One packet and one write, so do not make core1 wait out the
             * hold's two seconds. */
            flash_window_release();
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
            return ERR_OK;

        } else if (strcmp(path, "/api/config") == 0 && content_length > 0 && content_length < 512) {
            /* Config update with state-based safety check */
            DBG("POST /api/config cl=%lu", (unsigned long)content_length);
            char cfgbuf[512];
            uint16_t len = (body_in_first < content_length) ? body_in_first : content_length;
            pbuf_copy_partial(p, cfgbuf, len, body_offset);
            pbuf_free(p);
            cfgbuf[len] = '\0';

            /* Check if we're in a safe state for config changes */
            extern flight_state_t flight_get_state(void);
            extern flight_context_t *flight_get_context(void);
            extern int flight_config_reload(flight_context_t *);
            flight_state_t state = flight_get_state();

            const char *resp;
            if (state != PAD_IDLE) {
                /* Reject config changes - device not ready */
                DBG("POST /api/config REJECT state=%u (need PAD_IDLE=3)", (unsigned)state);
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg),
                         "HTTP/1.1 409 Conflict\r\n" CORS_HDR "Connection: close\r\n"
                         "Content-Type: application/json\r\n\r\n"
                         "{\"error\":\"Device not ready (state=%s)\","
                         "\"state\":\"%s\",\"reboot_required\":true}",
                         state_names[state < 7 ? state : 0], state_names[state < 7 ? state : 0]);
                tcp_write(pcb, err_msg, strlen(err_msg), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_sent(pcb, on_sent);
                tcp_arg(pcb, NULL);
                return ERR_OK;
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
                    flash_window_release();
                    resp = "HTTP/1.1 500 Error\r\n" CORS_HDR "Connection: close\r\n"
                           "Content-Type: application/json\r\n\r\n"
                           "{\"error\":\"Merged config exceeds the 512-byte budget\"}";
                    tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
                    tcp_output(pcb);
                    tcp_sent(pcb, on_sent);
                    tcp_arg(pcb, NULL);
                    return ERR_OK;
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
                            resp = "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n"
                                   "Content-Type: application/json\r\n\r\n"
                                   "{\"status\":\"ok\",\"applied\":true}";
                        } else {
                            DBG("POST /api/config WARN reload_result=%d", reload_result);
                            resp = "HTTP/1.1 500 Error\r\n" CORS_HDR "Connection: close\r\n"
                                   "Content-Type: application/json\r\n\r\n"
                                   "{\"error\":\"Config saved but reload failed\","
                                   "\"reboot_required\":true}";
                        }
                    } else {
                        DBG("POST /api/config FAIL file_open");
                        lfs_unmount(&lfs);
                        resp = "HTTP/1.1 500 Error\r\n" CORS_HDR "Connection: close\r\n\r\nFile open failed";
                    }
                } else {
                    DBG("POST /api/config FAIL lfs_mount");
                    resp = "HTTP/1.1 500 Error\r\n" CORS_HDR "Connection: close\r\n\r\nMount failed";
                }
            }
            flash_window_release(); /* the config write is one packet and done */
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
        } else if (strcmp(path, "/api/pins") == 0 && content_length > 0 && content_length < PIN_STORE_MAX) {
            DBG("POST /api/pins cl=%lu", (unsigned long)content_length);
            static char pinbuf[PIN_STORE_MAX];
            uint16_t len = (body_in_first < content_length) ? body_in_first : (uint16_t)content_length;
            pbuf_copy_partial(p, pinbuf, len, body_offset);
            pbuf_free(p);
            pinbuf[len] = '\0';

            extern flight_state_t flight_get_state(void);
            flight_state_t st = flight_get_state();

            char resp[320];
            if (st != PAD_IDLE) {
                /* Same interlock as /api/config: a pin map that changes under
                 * a flying board would move the pyro pins mid-flight. */
                snprintf(resp, sizeof(resp),
                         "HTTP/1.1 409 Conflict\r\n" CORS_HDR "Connection: close\r\n"
                         "Content-Type: application/json\r\n\r\n"
                         "{\"error\":\"Device not ready (state=%s)\",\"reboot_required\":true}",
                         state_names[st < 7 ? st : 0]);
            } else {
                /* Merged over the live assignment for the same reason
                 * /api/config merges (CFG-06): a partial post must not
                 * silently release a channel by omitting its key. */
                pin_assign_t merged = *pin_store_current();
                pin_assign_parse_ini(pinbuf, &merged);

                pin_verdict_t v = pin_store_save(&merged);
                if (v.err == PIN_OK) {
                    snprintf(resp, sizeof(resp),
                             "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n"
                             "Content-Type: application/json\r\n\r\n"
                             "{\"status\":\"ok\",\"reboot_required\":true}");
                } else {
                    char esc[160];
                    json_escape(esc, (int)sizeof(esc), v.what, (int)strlen(v.what));
                    snprintf(resp, sizeof(resp),
                             "HTTP/1.1 400 Bad Request\r\n" CORS_HDR "Connection: close\r\n"
                             "Content-Type: application/json\r\n\r\n"
                             "{\"error\":\"%s\",\"pin\":%u,\"code\":%d}",
                             esc, (unsigned)v.pin, (int)v.err);
                }
            }
            flash_window_release();
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            tcp_arg(pcb, NULL);
            return ERR_OK;
        } else if (strcmp(path, "/api/reboot") == 0) {
            DBG("POST /api/reboot");
            pbuf_free(p);
            extern volatile uint8_t pending_reset;
            pending_reset = 2;
            const char *resp = "HTTP/1.1 200 OK\r\n" CORS_HDR "Connection: close\r\n\r\nRebooting";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
        } else {
            pbuf_free(p);
            const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nBad request";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
        }

        tcp_output(pcb);
        tcp_sent(pcb, on_sent);
        tcp_arg(pcb, NULL);
        return ERR_OK;
    }

    pbuf_free(p);
    const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nBad request";
    tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    tcp_sent(pcb, on_sent);
    tcp_arg(pcb, NULL);
    return ERR_OK;
}

extern volatile uint32_t net_http_accept;
extern volatile uint32_t net_http_err;
extern volatile uint32_t net_conn_full;

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    (void)err;
    net_http_accept++;
    tcp_nagle_disable(pcb); /* Fix 2: disable Nagle for snappy HTTP */
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    return ERR_OK;
}

void http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 80);
    pcb = tcp_listen_with_backlog(pcb, 8);
    tcp_accept(pcb, on_accept);
}
