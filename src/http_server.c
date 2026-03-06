/*
 * Minimal HTTP server for file management over lwIP raw TCP.
 */
#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>
#include <lfs.h>
#include <pico/stdlib.h>
#include "device_status.h"

extern const struct lfs_config lfs_pico_flash_config;

static const char *state_names[] = {"PAD_IDLE", "ASCENT", "DESCENT", "LANDED"};

static const char *HEADER_200 =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";

static const char *HEADER_JSON =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n";

static const char *PAGE_HEAD =
    "<!DOCTYPE html><html><head><title>Pyro MK1B</title>"
    "<style>body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:0 20px}"
    "a{color:#07f}table{border-collapse:collapse;width:100%}"
    "td,th{text-align:left;padding:4px 8px;border-bottom:1px solid #ddd}"
    "</style></head><body><h2>Pyro MK1B</h2>"
    "<div id=s></div><hr>"
    "<script>function u(){fetch('/api/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('s').innerHTML="
    "'<b>'+d.state+'</b> Alt:'+(d.alt_cm/100).toFixed(1)"
    "+'m Spd:'+(d.vspeed_cms/100).toFixed(1)+'m/s'"
    "+'<br>P1:'+(d.pyro1_fired?'FIRED':d.pyro1_cont?'OK':'OPEN')+'('+d.pyro1_adc+')'"
    "+' P2:'+(d.pyro2_fired?'FIRED':d.pyro2_cont?'OK':'OPEN')+'('+d.pyro2_adc+')'"
    "+' Up:'+(d.uptime/1000).toFixed(0)+'s'"
    "+' Pa:'+d.pressure_pa"
    "}).catch(()=>{});setTimeout(u,500)}u()</script>";

static const char *PAGE_TAIL = "</body></html>";

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)arg; (void)len;
    tcp_close(pcb);
    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;

    if (!p) {
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Parse first line of request */
    char req[128] = {0};
    uint16_t tot = p->tot_len;
    uint16_t cpy = tot < sizeof(req) - 1 ? tot : sizeof(req) - 1;
    pbuf_copy_partial(p, req, cpy, 0);
    pbuf_free(p);
    tcp_recved(pcb, tot);

    /* Build response */
    char buf[4096];
    int pos = 0;

    if (strncmp(req, "GET /api/status", 15) == 0) {
        /* JSON status endpoint */
        const char *sn = (g_status.state < 4) ? state_names[g_status.state] : "UNKNOWN";
        pos = snprintf(buf, sizeof(buf),
            "%s{\"state\":\"%s\",\"alt_cm\":%ld,\"max_alt_cm\":%ld,"
            "\"vspeed_cms\":%ld,\"pressure_pa\":%ld,"
            "\"pyro1_cont\":%s,\"pyro2_cont\":%s,"
            "\"pyro1_adc\":%u,\"pyro2_adc\":%u,"
            "\"pyro1_fired\":%s,\"pyro2_fired\":%s,"
            "\"armed\":%s,\"under_thrust\":%s,"
            "\"flight_ms\":%lu,\"uptime\":%lu}",
            HEADER_JSON, sn,
            (long)g_status.altitude_cm, (long)g_status.max_altitude_cm,
            (long)g_status.vertical_speed_cms, (long)g_status.pressure_pa,
            g_status.pyro1_continuity ? "true" : "false",
            g_status.pyro2_continuity ? "true" : "false",
            (unsigned)g_status.pyro1_adc, (unsigned)g_status.pyro2_adc,
            g_status.pyro1_fired ? "true" : "false",
            g_status.pyro2_fired ? "true" : "false",
            g_status.pyros_armed ? "true" : "false",
            g_status.under_thrust ? "true" : "false",
            (unsigned long)g_status.flight_time_ms,
            (unsigned long)to_ms_since_boot(get_absolute_time()));
        tcp_write(pcb, buf, pos, TCP_WRITE_FLAG_COPY);

    } else if (strncmp(req, "GET /files/", 11) == 0) {
        /* Download file */
        char filename[32] = "/";
        char *end = strchr(req + 11, ' ');
        if (end) {
            int flen = end - (req + 11);
            if (flen > 28) flen = 28;
            memcpy(filename + 1, req + 11, flen);
            filename[1 + flen] = '\0';
        }

        lfs_t lfs;
        lfs_file_t file;
        if (lfs_mount(&lfs, &lfs_pico_flash_config) == LFS_ERR_OK) {
            if (lfs_file_open(&lfs, &file, filename, LFS_O_RDONLY) == LFS_ERR_OK) {
                struct lfs_info info;
                lfs_stat(&lfs, filename, &info);
                pos = snprintf(buf, sizeof(buf),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Content-Disposition: attachment; filename=\"%s\"\r\n"
                    "Content-Length: %lu\r\n"
                    "Connection: close\r\n\r\n", filename + 1, (unsigned long)info.size);
                tcp_write(pcb, buf, pos, TCP_WRITE_FLAG_COPY);

                lfs_ssize_t n;
                while ((n = lfs_file_read(&lfs, &file, buf, sizeof(buf))) > 0) {
                    tcp_write(pcb, buf, n, TCP_WRITE_FLAG_COPY);
                }
                lfs_file_close(&lfs, &file);
            } else {
                pos = snprintf(buf, sizeof(buf), "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot found");
                tcp_write(pcb, buf, pos, TCP_WRITE_FLAG_COPY);
            }
            lfs_unmount(&lfs);
        }
    } else {
        /* File listing page */
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s", HEADER_200, PAGE_HEAD);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<table><tr><th>File</th><th>Size</th><th></th></tr>");

        lfs_t lfs;
        if (lfs_mount(&lfs, &lfs_pico_flash_config) == LFS_ERR_OK) {
            lfs_dir_t dir;
            if (lfs_dir_open(&lfs, &dir, "/") == LFS_ERR_OK) {
                struct lfs_info info;
                while (lfs_dir_read(&lfs, &dir, &info) > 0) {
                    if (info.type != LFS_TYPE_REG) continue;
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "<tr><td>%s</td><td>%lu</td>"
                        "<td><a href=\"/files/%s\">download</a></td></tr>",
                        info.name, (unsigned long)info.size, info.name);
                }
                lfs_dir_close(&lfs, &dir);
            }
            lfs_unmount(&lfs);
        }

        pos += snprintf(buf + pos, sizeof(buf) - pos, "</table>%s", PAGE_TAIL);
        tcp_write(pcb, buf, pos, TCP_WRITE_FLAG_COPY);
    }

    tcp_output(pcb);
    tcp_sent(pcb, http_sent);
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)err;
    tcp_recv(pcb, http_recv);
    return ERR_OK;
}

void http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 80);
    pcb = tcp_listen_with_backlog(pcb, 4);
    tcp_accept(pcb, http_accept);
}
