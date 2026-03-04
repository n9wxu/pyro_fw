/*
 * Streaming HTTP server: serves files from littlefs /www/, API endpoints.
 * Handles any file size via chunked read/write with per-connection state.
 */
#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>
#include <lfs.h>
#include <pico/stdlib.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <pico_fota_bootloader/core.h>
#include "device_status.h"

#define FW_VERSION "1.0.0"

extern const struct lfs_config lfs_pico_flash_config;

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
    lfs_t        lfs;
    lfs_file_t   file;
    bool         lfs_mounted;
    bool         file_open;
    uint32_t     remaining;     /* bytes left to receive */
    char         path[64];
} conn_state_t;

static conn_state_t conn_pool[4];

static conn_state_t *conn_alloc(void) {
    for (int i = 0; i < 4; i++)
        if (conn_pool[i].phase == CONN_IDLE) {
            memset(&conn_pool[i], 0, sizeof(conn_state_t));
            return &conn_pool[i];
        }
    return NULL;
}

static void conn_free(conn_state_t *cs) {
    if (cs->file_open) { lfs_file_close(&cs->lfs, &cs->file); cs->file_open = false; }
    if (cs->lfs_mounted) { lfs_unmount(&cs->lfs); cs->lfs_mounted = false; }
    cs->phase = CONN_IDLE;
}

/* ── OTA firmware update state ────────────────────────────────────── */

/* Download slot flash offset (from linker symbols) */
extern uint32_t __FLASH_DOWNLOAD_SLOT_START;
#define OTA_SLOT_OFF ((uint32_t)&__FLASH_DOWNLOAD_SLOT_START - XIP_BASE)

static uint8_t  ota_buf[FLASH_SECTOR_SIZE] __attribute__((aligned(FLASH_PAGE_SIZE)));
static uint32_t ota_offset;      /* bytes written so far */
static uint16_t ota_buf_fill;

static void ota_flush(void) {
    if (ota_buf_fill == 0) return;
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
}

static void ota_write(const void *data, uint16_t len) {
    const uint8_t *src = (const uint8_t *)data;
    while (len > 0) {
        uint16_t space = FLASH_SECTOR_SIZE - ota_buf_fill;
        uint16_t chunk = (len < space) ? len : space;
        memcpy(ota_buf + ota_buf_fill, src, chunk);
        ota_buf_fill += chunk;
        src += chunk;
        len -= chunk;
        if (ota_buf_fill == FLASH_SECTOR_SIZE)
            ota_flush();
    }
}

/* ── Content type ─────────────────────────────────────────────────── */

static const char *content_type_hdr(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".csv") == 0) return "text/csv";
    return "application/octet-stream";
}

/* ── Send next chunk of file (called from sent callback) ──────────── */

static void send_next_chunk(struct tcp_pcb *pcb, conn_state_t *cs) {
    char buf[CHUNK_SIZE];
    lfs_ssize_t n = lfs_file_read(&cs->lfs, &cs->file, buf, CHUNK_SIZE);
    if (n > 0) {
        tcp_write(pcb, buf, n, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
    } else {
        conn_free(cs);
        tcp_close(pcb);
    }
}

/* ── API handlers ─────────────────────────────────────────────────── */

static const char *state_names[] = {"PAD_IDLE", "ASCENT", "DESCENT", "LANDED"};

static void serve_api_status(struct tcp_pcb *pcb) {
    char buf[512];
    const char *sn = (g_status.state < 4) ? state_names[g_status.state] : "UNKNOWN";
    int pos = snprintf(buf, sizeof(buf),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
        "{\"state\":\"%s\",\"alt_cm\":%ld,\"max_alt_cm\":%ld,"
        "\"vspeed_cms\":%ld,\"pressure_pa\":%ld,"
        "\"pyro1_cont\":%s,\"pyro2_cont\":%s,"
        "\"pyro1_adc\":%u,\"pyro2_adc\":%u,"
        "\"pyro1_fired\":%s,\"pyro2_fired\":%s,"
        "\"armed\":%s,\"flight_ms\":%lu,\"uptime\":%lu,\"fw_version\":\"%s\"}",
        sn,
        (long)g_status.altitude_cm, (long)g_status.max_altitude_cm,
        (long)g_status.vertical_speed_cms, (long)g_status.pressure_pa,
        g_status.pyro1_continuity ? "true" : "false",
        g_status.pyro2_continuity ? "true" : "false",
        (unsigned)g_status.pyro1_adc, (unsigned)g_status.pyro2_adc,
        g_status.pyro1_fired ? "true" : "false",
        g_status.pyro2_fired ? "true" : "false",
        g_status.pyros_armed ? "true" : "false",
        (unsigned long)g_status.flight_time_ms,
        (unsigned long)to_ms_since_boot(get_absolute_time()),
        FW_VERSION);
    tcp_write(pcb, buf, pos, TCP_WRITE_FLAG_COPY);
}

static void serve_api_config(struct tcp_pcb *pcb) {
    lfs_t lfs;
    lfs_file_t file;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) != LFS_ERR_OK) goto fallback;
    if (lfs_file_open(&lfs, &file, "config.ini", LFS_O_RDONLY) != LFS_ERR_OK) {
        lfs_unmount(&lfs);
        goto fallback;
    }
    {
        const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        tcp_write(pcb, hdr, strlen(hdr), TCP_WRITE_FLAG_COPY);
        char buf[256];
        lfs_ssize_t n;
        while ((n = lfs_file_read(&lfs, &file, buf, sizeof(buf))) > 0)
            tcp_write(pcb, buf, n, TCP_WRITE_FLAG_COPY);
        lfs_file_close(&lfs, &file);
        lfs_unmount(&lfs);
        return;
    }
fallback:;
    const char *resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNo config.ini";
    tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
}

static void serve_api_flight_csv(struct tcp_pcb *pcb) {
    const char *resp =
        "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\n"
        "Content-Disposition: attachment; filename=\"flight.csv\"\r\n"
        "Connection: close\r\n\r\n"
        "time_ms,pressure_pa,altitude_cm,state\r\n";
    tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
    /* TODO: stream flight data from raw file */
}

/* ── Default page if /www/index.html missing ──────────────────────── */

static const char *DEFAULT_PAGE =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
    "<!DOCTYPE html><html><body><h2>Pyro MK1B</h2>"
    "<p>No web files uploaded. POST files to /www/ to set up the UI.</p>"
    "<p><a href=\"/api/status\">Status JSON</a></p></body></html>";

/* ── TCP callbacks ────────────────────────────────────────────────── */

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)len;
    conn_state_t *cs = (conn_state_t *)arg;
    if (cs && cs->phase == CONN_SENDING_FILE) {
        send_next_chunk(pcb, cs);
    } else {
        if (cs) conn_free(cs);
        tcp_close(pcb);
    }
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err;
    conn_state_t *cs = (conn_state_t *)arg;

    if (!p) {
        if (cs) conn_free(cs);
        tcp_close(pcb);
        return ERR_OK;
    }

    /* ── Receiving file upload (continuation packets) ──────────── */
    if (cs && cs->phase == CONN_RECEIVING_FILE) {
        char buf[CHUNK_SIZE];
        uint16_t off = 0;
        while (off < p->tot_len && cs->remaining > 0) {
            uint16_t chunk = p->tot_len - off;
            if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
            if (chunk > cs->remaining) chunk = cs->remaining;
            pbuf_copy_partial(p, buf, chunk, off);
            lfs_file_write(&cs->lfs, &cs->file, buf, chunk);
            off += chunk;
            cs->remaining -= chunk;
        }
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);

        if (cs->remaining == 0) {
            conn_free(cs);
            const char *resp = "HTTP/1.1 201 Created\r\nConnection: close\r\n\r\nOK";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
        }
        return ERR_OK;
    }

    /* ── Receiving OTA firmware (continuation packets) ─────────── */
    if (cs && cs->phase == CONN_RECEIVING_OTA) {
        char buf[CHUNK_SIZE];
        uint16_t off = 0;
        while (off < p->tot_len && cs->remaining > 0) {
            uint16_t chunk = p->tot_len - off;
            if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
            if (chunk > cs->remaining) chunk = cs->remaining;
            pbuf_copy_partial(p, buf, chunk, off);
            ota_write(buf, chunk);
            off += chunk;
            cs->remaining -= chunk;
        }
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);

        if (cs->remaining == 0) {
            ota_flush();
            conn_free(cs);
            pfb_mark_download_slot_as_valid();
            const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOTA OK, rebooting...";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_sent(pcb, on_sent);
            pfb_perform_update();  /* reboots via watchdog */
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
        int mlen = sp1 - hdr; if (mlen > 7) mlen = 7;
        memcpy(method, hdr, mlen);
        sp1++;
        char *sp2 = memchr(sp1, ' ', hdr_len - (sp1 - hdr));
        if (sp2) {
            int plen = sp2 - sp1; if (plen > 63) plen = 63;
            memcpy(path, sp1, plen);
        }
    }

    /* Parse Content-Length */
    uint32_t content_length = 0;
    const char *cl = strstr(hdr, "Content-Length: ");
    if (!cl) cl = strstr(hdr, "content-length: ");
    if (cl) content_length = atoi(cl + 16);

    /* Find body start */
    const char *body_start = strstr(hdr, "\r\n\r\n");
    uint16_t body_offset = body_start ? (body_start + 4 - hdr) : p->tot_len;
    uint16_t body_in_first = (p->tot_len > body_offset) ? p->tot_len - body_offset : 0;

    tcp_recved(pcb, p->tot_len);

    /* ── Route: GET ────────────────────────────────────────────── */
    if (strcmp(method, "GET") == 0) {
        pbuf_free(p);

        if (strcmp(path, "/api/status") == 0) {
            serve_api_status(pcb);
        } else if (strcmp(path, "/api/config") == 0) {
            serve_api_config(pcb);
        } else if (strcmp(path, "/api/flight.csv") == 0) {
            serve_api_flight_csv(pcb);
        } else {
            /* Serve file from /www/ */
            const char *fpath = path;
            if (strcmp(path, "/") == 0) fpath = "/www/index.html";

            cs = conn_alloc();
            if (!cs) { tcp_close(pcb); return ERR_OK; }

            if (lfs_mount(&cs->lfs, &lfs_pico_flash_config) != LFS_ERR_OK) {
                conn_free(cs);
                tcp_write(pcb, DEFAULT_PAGE, strlen(DEFAULT_PAGE), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb); tcp_sent(pcb, on_sent); tcp_arg(pcb, NULL);
                return ERR_OK;
            }
            cs->lfs_mounted = true;

            if (lfs_file_open(&cs->lfs, &cs->file, fpath, LFS_O_RDONLY) != LFS_ERR_OK) {
                conn_free(cs);
                if (strcmp(path, "/") == 0) {
                    tcp_write(pcb, DEFAULT_PAGE, strlen(DEFAULT_PAGE), TCP_WRITE_FLAG_COPY);
                } else {
                    const char *r404 = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot found";
                    tcp_write(pcb, r404, strlen(r404), TCP_WRITE_FLAG_COPY);
                }
                tcp_output(pcb); tcp_sent(pcb, on_sent); tcp_arg(pcb, NULL);
                return ERR_OK;
            }
            cs->file_open = true;
            cs->phase = CONN_SENDING_FILE;
            tcp_arg(pcb, cs);

            /* Send header */
            char resp_hdr[128];
            int hlen = snprintf(resp_hdr, sizeof(resp_hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
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
            if (!cs) { pbuf_free(p); tcp_close(pcb); return ERR_OK; }

            cs->phase = CONN_RECEIVING_OTA;
            cs->remaining = content_length;
            tcp_arg(pcb, cs);

            pfb_firmware_commit();
            ota_offset = 0;
            ota_buf_fill = 0;

            if (body_in_first > 0) {
                char buf[CHUNK_SIZE];
                uint16_t off = body_offset;
                while (off < p->tot_len && cs->remaining > 0) {
                    uint16_t chunk = p->tot_len - off;
                    if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
                    if (chunk > cs->remaining) chunk = cs->remaining;
                    pbuf_copy_partial(p, buf, chunk, off);
                    ota_write(buf, chunk);
                    off += chunk;
                    cs->remaining -= chunk;
                }
            }
            pbuf_free(p);

            if (cs->remaining == 0) {
                ota_flush();
                conn_free(cs);
                pfb_mark_download_slot_as_valid();
                const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOTA OK, rebooting...";
                tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb); tcp_sent(pcb, on_sent); tcp_arg(pcb, NULL);
                pfb_perform_update();
            }
            return ERR_OK;

        } else if (strncmp(path, "/www/", 5) == 0 && content_length > 0) {
            cs = conn_alloc();
            if (!cs) { pbuf_free(p); tcp_close(pcb); return ERR_OK; }

            if (lfs_mount(&cs->lfs, &lfs_pico_flash_config) != LFS_ERR_OK) {
                conn_free(cs); pbuf_free(p); tcp_close(pcb); return ERR_OK;
            }
            cs->lfs_mounted = true;
            lfs_mkdir(&cs->lfs, "/www");

            if (lfs_file_open(&cs->lfs, &cs->file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != LFS_ERR_OK) {
                conn_free(cs); pbuf_free(p); tcp_close(pcb); return ERR_OK;
            }
            cs->file_open = true;
            cs->phase = CONN_RECEIVING_FILE;
            cs->remaining = content_length;
            tcp_arg(pcb, cs);

            /* Write body data from first packet */
            if (body_in_first > 0) {
                char buf[CHUNK_SIZE];
                uint16_t off = body_offset;
                while (off < p->tot_len && cs->remaining > 0) {
                    uint16_t chunk = p->tot_len - off;
                    if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
                    if (chunk > cs->remaining) chunk = cs->remaining;
                    pbuf_copy_partial(p, buf, chunk, off);
                    lfs_file_write(&cs->lfs, &cs->file, buf, chunk);
                    off += chunk;
                    cs->remaining -= chunk;
                }
            }
            pbuf_free(p);

            if (cs->remaining == 0) {
                conn_free(cs);
                const char *resp = "HTTP/1.1 201 Created\r\nConnection: close\r\n\r\nOK";
                tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb); tcp_sent(pcb, on_sent); tcp_arg(pcb, NULL);
            }
            return ERR_OK;

        } else if (strcmp(path, "/api/config") == 0) {
            pbuf_free(p);
            /* TODO: parse + validate + store config */
            const char *resp = "HTTP/1.1 201 Created\r\nConnection: close\r\n\r\nOK";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
        } else {
            pbuf_free(p);
            const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nBad request";
            tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
        }

        tcp_output(pcb); tcp_sent(pcb, on_sent); tcp_arg(pcb, NULL);
        return ERR_OK;
    }

    pbuf_free(p);
    const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nBad request";
    tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb); tcp_sent(pcb, on_sent); tcp_arg(pcb, NULL);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)err;
    tcp_recv(pcb, on_recv);
    return ERR_OK;
}

void http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 80);
    pcb = tcp_listen_with_backlog(pcb, 4);
    tcp_accept(pcb, on_accept);
}
