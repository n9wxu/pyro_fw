/*
 * An SD card in SPI mode. See sd_card.h.
 *
 * The command framing and the flow follow the specification's section 7
 * (PDF pages 227-247) and ChaN's generic MMC/SD driver shape.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sd_card.h"
#include "spi_bus.h"
#include "crc16.h"
#include "board_pins.h"
#include "rtos_tasks.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include <stdio.h>
#include <string.h>

/* The identification clock, 400 kHz at most. */
#define INIT_HZ 400000u

/* PDF page 97: reads 100 ms; write busy 250 ms, 500 ms on SDXC. ACMD41's
 * initialisation, a second. */
#define READ_TIMEOUT_US 100000u
#define WRITE_TIMEOUT_US 500000u
#define INIT_TIMEOUT_US 1000000u

/* A busy wait gives the bus back after this, between polls. */
#define YIELD_AFTER_US 1000u
#define SD_INIT_RESTARTS 3u

#define RETRIES 3

#define ACMD 0x80u

static sd_type_t type;
static uint32_t sectors;
static bool mounted;
static uint32_t mount_count;
static FATFS fatfs;
static sd_stats_t st;
static uint32_t data_hz;
static bool use_crc = true;
static uint32_t init_timeout_us = INIT_TIMEOUT_US;
static uint32_t poll_gap_ms;
static uint32_t init_restarts = SD_INIT_RESTARTS;
static uint32_t data_hz_asked = BOARD_SD_SPI_HZ;
static char cid_hex[48];

/* CRC7 over a command's first five bytes, with the end bit. */
static uint8_t crc7(const uint8_t *d, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = d[i];
        for (int j = 0; j < 8; j++) {
            c = (uint8_t)(c << 1);
            if ((b ^ c) & 0x80u)
                c ^= 0x09u;
            b = (uint8_t)(b << 1);
        }
    }
    return (uint8_t)((c << 1) | 1u);
}

static void select(void) {
    gpio_put(BOARD_PIN_SD_CS, 0);
    spi_bus_byte(0xFF);
}

/* A card releases its data line on the clock after CS rises. */
static void deselect(void) {
    gpio_put(BOARD_PIN_SD_CS, 1);
    spi_bus_byte(0xFF);
}

/* Until the card reads 0xFF, not busy. Past YIELD_AFTER_US the bus goes back
 * between polls: deselected, a card keeps programming and shows busy again
 * when selected. */
static bool wait_ready(uint32_t timeout_us) {
    uint32_t t0 = time_us_32();
    for (;;) {
        if (spi_bus_byte(0xFF) == 0xFF) {
            uint32_t d = time_us_32() - t0;
            if (d > st.busy_max_us)
                st.busy_max_us = d;
            return true;
        }
        uint32_t el = time_us_32() - t0;
        if (el > timeout_us) {
            st.timeouts++;
            return false;
        }
        if (el > YIELD_AFTER_US && rtos_running()) {
            if (data_hz == 0)
                st.init_yields++;
            deselect();
            spi_bus_give();
            vTaskDelay(1);
            if (!spi_bus_take(1000)) {
                st.timeouts++;
                return false;
            }
            spi_bus_setup(data_hz ? data_hz : INIT_HZ, 0, 0);
            select();
        }
    }
}

/* R1, or 0xFF for no answer. Selects the card; the caller deselects. */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    if (cmd & ACMD) {
        cmd &= (uint8_t)~ACMD;
        uint8_t r = send_cmd(55, 0);
        if (r > 1)
            return r;
    }
    if (cmd != 12) {
        deselect();
        select();
        /* The card may still be programming the last write's block. */
        if (cmd != 0 && !wait_ready(WRITE_TIMEOUT_US))
            return 0xFF;
    }
    uint8_t f[6] = {(uint8_t)(0x40u | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                    (uint8_t)(arg >> 8),    (uint8_t)arg,         0};
    f[5] = crc7(f, 5);
    spi_bus_xfer(f, NULL, 6);
    if (cmd == 12)
        spi_bus_byte(0xFF); /* the stuff byte after CMD12 */
    uint8_t r = 0xFF;
    for (int i = 0; i < 10; i++) {
        r = spi_bus_byte(0xFF);
        if (!(r & 0x80u))
            break;
    }
    return r;
}

/* A data block after its start token 0xFE, then its CRC16 (PDF pages 230,
 * 246). */
static bool rcvr_block(uint8_t *buf, uint32_t n) {
    uint32_t t0 = time_us_32();
    uint8_t tok;
    do {
        tok = spi_bus_byte(0xFF);
    } while (tok == 0xFF && time_us_32() - t0 < READ_TIMEOUT_US);
    if (tok != 0xFE) {
        if (tok == 0xFF)
            st.timeouts++;
        else
            st.cmd_errors++;
        return false;
    }
    spi_bus_xfer(NULL, buf, n);
    uint8_t c[2];
    spi_bus_xfer(NULL, c, 2);
    if (crc16(buf, n) != (uint16_t)((c[0] << 8) | c[1])) {
        st.crc_errors++;
        return false;
    }
    return true;
}

/* One block after its token -- 0xFE single, 0xFC multiple -- or the stop
 * token 0xFD alone. The data response xxx0 0101 is accepted, xxx0 1011 a CRC
 * error (PDF page 246). */
static bool xmit_block(const uint8_t *buf, uint8_t token) {
    if (!wait_ready(WRITE_TIMEOUT_US))
        return false;
    spi_bus_byte(token);
    if (token == 0xFD)
        return true;
    spi_bus_xfer(buf, NULL, SD_SECTOR);
    uint16_t c = crc16(buf, SD_SECTOR);
    spi_bus_byte((uint8_t)(c >> 8));
    spi_bus_byte((uint8_t)c);
    uint8_t resp = spi_bus_byte(0xFF);
    if ((resp & 0x1Fu) != 0x05u) {
        if ((resp & 0x1Fu) == 0x0Bu)
            st.crc_errors++;
        else
            st.cmd_errors++;
        return false;
    }
    return true;
}

static void hexify(const uint8_t *b, int n, char *out) {
    for (int i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02X", b[i]);
}

/* CSD version 2: C_SIZE in bytes 7-9; version 1: C_SIZE, C_SIZE_MULT and
 * READ_BL_LEN (the specification's section 5.3). */
static uint32_t csd_sectors(const uint8_t *csd) {
    if ((csd[0] >> 6) == 1) {
        uint32_t cs = ((uint32_t)(csd[7] & 0x3Fu) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        return (cs + 1u) << 10;
    }
    uint32_t bl = csd[5] & 15u;
    uint32_t cs = ((uint32_t)(csd[6] & 3u) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
    uint32_t mult = ((csd[9] & 3u) << 1) | (csd[10] >> 7);
    return (cs + 1u) << (mult + 2u + bl - 9u);
}

int sd_init_card(void) {
    if (!spi_bus_take(2000))
        return -1;
    spi_bus_setup(INIT_HZ, 0, 0);
    data_hz = 0;
    type = SD_NONE;
    /* At least 74 clocks with CS high before the first command (PDF pages
     * 221-222). */
    gpio_put(BOARD_PIN_SD_CS, 1);
    spi_bus_xfer(NULL, NULL, 10);

    sd_type_t ty = SD_NONE;
    memset(st.init_r1, 0xEE, sizeof(st.init_r1));
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && r != 1; i++)
        r = send_cmd(0, 0);
    st.init_r1[0] = r;
    if (r == 1) {
        uint8_t r8 = send_cmd(8, 0x1AA);
        st.init_r1[1] = r8;
        if (r8 == 1) {
            uint8_t *r7 = st.init_r7;
            spi_bus_xfer(NULL, r7, 4);
            if (r7[2] == 0x01 && r7[3] == 0xAA) {
                st.init_r1[2] = use_crc ? send_cmd(59, 1) : 0xCC; /* CRC on, before ACMD41 (PDF page 230) */
                uint32_t t0 = time_us_32();
                uint8_t r41;
                st.acmd41_polls = 0;
                st.cmd55_first = send_cmd(55, 0);
                st.acmd41_first = send_cmd(41, 1u << 30);
                r41 = st.acmd41_first;
                st.acmd41_ones = 0;
                st.acmd41_other_ms = 0;
                st.acmd41_other = 0;
                st.after_r58 = st.after_r0 = 0xEE;
                st.restarts = 0;
                memset(st.fail_ms, 0, sizeof(st.fail_ms));
                while (r41 != 0 && time_us_32() - t0 < init_timeout_us) {
                    spi_bus_xfer(NULL, NULL, 8);
                    if (poll_gap_ms && rtos_running()) {
                        deselect();
                        vTaskDelay(pdMS_TO_TICKS(poll_gap_ms));
                    }
                    r41 = send_cmd(ACMD | 41, 1u << 30);
                    st.acmd41_polls++;
                    if (r41 == 0 || r41 == 1) {
                        st.acmd41_ones += r41;
                        continue;
                    }
                    uint32_t ms = (time_us_32() - t0) / 1000u + 1u;
                    if (!st.acmd41_other_ms) {
                        st.acmd41_other_ms = ms;
                        st.acmd41_other = r41;
                    }
                    if (st.restarts < sizeof(st.fail_ms) / sizeof(st.fail_ms[0]))
                        st.fail_ms[st.restarts] = (uint16_t)ms;
                    uint8_t ocr[4];
                    uint8_t r58 = send_cmd(58, 0);
                    spi_bus_xfer(NULL, ocr, 4);
                    uint8_t r0 = send_cmd(0, 0);
                    if (st.after_r0 == 0xEE) {
                        st.after_r58 = r58;
                        st.after_r0 = r0;
                    }
                    if (r0 != 1 || st.restarts >= init_restarts)
                        break;
                    if (st.restarts < 255u)
                        st.restarts++;
                    if (send_cmd(8, 0x1AA) != 1)
                        break;
                    spi_bus_xfer(NULL, ocr, 4);
                    if (use_crc)
                        send_cmd(59, 1);
                }
                st.init_r1[3] = r41;
                uint8_t r58 = 0xEE;
                if (r41 == 0 && (r58 = send_cmd(58, 0)) == 0) {
                    uint8_t *ocr = st.init_ocr;
                    spi_bus_xfer(NULL, ocr, 4);
                    ty = (ocr[0] & 0x40u) ? SD_V2_HC : SD_V2_SC;
                }
                st.init_r1[4] = r58;
            }
        } else {
            send_cmd(59, 1);
            uint32_t t0 = time_us_32();
            while (send_cmd(ACMD | 41, 0) != 0 && time_us_32() - t0 < INIT_TIMEOUT_US)
                spi_bus_xfer(NULL, NULL, 8);
            if (time_us_32() - t0 < INIT_TIMEOUT_US)
                ty = SD_V1;
        }
    }
    if (ty != SD_NONE && ty != SD_V2_HC && send_cmd(16, SD_SECTOR) != 0)
        ty = SD_NONE;
    uint8_t csd[16], cid[16];
    if (ty != SD_NONE && (st.init_r1[5] = send_cmd(9, 0)) == 0 && rcvr_block(csd, 16))
        sectors = csd_sectors(csd);
    if (ty != SD_NONE && send_cmd(10, 0) == 0 && rcvr_block(cid, 16))
        hexify(cid, 16, cid_hex);
    deselect();
    if (ty != SD_NONE) {
        data_hz = spi_bus_setup(data_hz_asked, 0, 0);
        st.hz = data_hz;
    }
    type = ty;
    spi_bus_give();
    return ty != SD_NONE ? 0 : -1;
}

int sd_read(uint8_t *buf, uint32_t lba, uint32_t count) {
    if (type == SD_NONE || count == 0)
        return -1;
    uint32_t addr = type == SD_V2_HC ? lba : lba * SD_SECTOR;
    if (!spi_bus_take(2000))
        return -1;
    spi_bus_setup(data_hz, 0, 0);
    bool ok = false;
    for (int attempt = 0; attempt < RETRIES && !ok; attempt++) {
        if (attempt)
            st.retries++;
        uint8_t *p = buf;
        uint32_t n = count;
        if (count == 1) {
            ok = send_cmd(17, addr) == 0 && rcvr_block(p, SD_SECTOR);
        } else if (send_cmd(18, addr) == 0) {
            while (n > 0 && rcvr_block(p, SD_SECTOR)) {
                p += SD_SECTOR;
                n--;
            }
            send_cmd(12, 0);
            ok = n == 0;
        } else {
            st.cmd_errors++;
        }
        deselect();
    }
    st.reads++;
    if (ok)
        st.sectors_read += count;
    spi_bus_give();
    return ok ? 0 : -1;
}

int sd_write(const uint8_t *buf, uint32_t lba, uint32_t count) {
    if (type == SD_NONE || count == 0)
        return -1;
    uint32_t addr = type == SD_V2_HC ? lba : lba * SD_SECTOR;
    uint32_t t0 = time_us_32();
    if (!spi_bus_take(2000))
        return -1;
    spi_bus_setup(data_hz, 0, 0);
    bool ok = false;
    for (int attempt = 0; attempt < RETRIES && !ok; attempt++) {
        if (attempt)
            st.retries++;
        const uint8_t *p = buf;
        uint32_t n = count;
        if (count == 1) {
            ok = send_cmd(24, addr) == 0 && xmit_block(p, 0xFE);
        } else {
            /* ACMD23: the blocks about to be written, which a card may erase
             * ahead of them. */
            send_cmd(ACMD | 23, count);
            if (send_cmd(25, addr) == 0) {
                while (n > 0 && xmit_block(p, 0xFC)) {
                    p += SD_SECTOR;
                    n--;
                }
                ok = xmit_block(NULL, 0xFD) && n == 0;
            } else {
                st.cmd_errors++;
            }
        }
        /* The card programs the last block after this: its busy is the next
         * command's wait, or sd_sync()'s. */
        deselect();
    }
    st.writes++;
    if (ok)
        st.sectors_written += count;
    spi_bus_give();
    uint32_t d = time_us_32() - t0;
    if (d > st.write_max_us)
        st.write_max_us = d;
    return ok ? 0 : -1;
}

int sd_sync(void) {
    if (type == SD_NONE)
        return -1;
    if (!spi_bus_take(2000))
        return -1;
    spi_bus_setup(data_hz, 0, 0);
    select();
    bool ok = wait_ready(WRITE_TIMEOUT_US);
    deselect();
    spi_bus_give();
    return ok ? 0 : -1;
}

uint32_t sd_sectors(void) {
    return sectors;
}

sd_type_t sd_type(void) {
    return type;
}

uint32_t sd_mount_count(void) {
    return mount_count;
}

bool sd_mounted(void) {
    return mounted;
}

uint8_t sd_bus_probe_imu(void) {
    if (!spi_bus_take(1000))
        return 0;
    spi_bus_setup(1000000u, 1, 1);
    uint8_t tx[2] = {0x80u | 0x0Fu, 0xFF}, rx[2] = {0, 0};
    gpio_put(BOARD_PIN_IMU_CS, 0);
    spi_bus_xfer(tx, rx, 2);
    gpio_put(BOARD_PIN_IMU_CS, 1);
    spi_bus_give();
    return rx[1];
}

void sd_set_crc(bool on) {
    use_crc = on;
}

void sd_set_init_timeout_ms(uint32_t ms) {
    init_timeout_us = ms * 1000u;
}

void sd_set_poll_gap_ms(uint32_t ms) {
    poll_gap_ms = ms;
}

void sd_set_init_restarts(uint32_t n) {
    init_restarts = n;
}

void sd_set_data_hz(uint32_t hz) {
    data_hz_asked = hz;
}

bool sd_clock_idle(uint32_t ms) {
    if (!spi_bus_take(2000))
        return false;
    spi_bus_setup(INIT_HZ, 0, 0);
    gpio_put(BOARD_PIN_SD_CS, 1);
    uint32_t t0 = time_us_32();
    while (time_us_32() - t0 < ms * 1000u)
        spi_bus_byte(0xFF);
    if (data_hz)
        spi_bus_setup(data_hz, 0, 0);
    spi_bus_give();
    return true;
}

int sd_start(void) {
    spi_bus_init();
    if (sd_init_card() != 0)
        return -1;
    FRESULT r = f_mount(&fatfs, "", 1);
    st.mount_rc = (uint32_t)r;
    mounted = r == FR_OK;
    if (mounted)
        mount_count++;
    return mounted ? 0 : -2;
}

void sd_get_stats(sd_stats_t *out) {
    *out = st;
}

const char *sd_cid(void) {
    return cid_hex;
}
