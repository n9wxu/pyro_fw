/*
 * Board identity: MAC, serial number, USB serial string and IP subnet.
 *
 * Boards must not share a MAC, a USB serial or an address. Three identical
 * boards on one host leave one usable board: the rest enumerate and are
 * ignored, and `picotool --ser` cannot target any of them.
 *
 * Drawn from the RNG once, then kept [DD-072]. A board with no /serial.txt
 * draws a MAC at boot (mac_random.h) and the storage path writes it there,
 * tagged "rng", so the address survives reboots and reflashes. The flash
 * chip's unique id cannot serve: MK1C's XT25F128F gives two boards the same
 * one (boards/mk1c/THEORY_OF_OPERATION.md, "Known limits").
 *
 * The subnet is the MAC's last byte, 8 bits, so by the birthday bound two
 * boards share one with about 4% probability at 5 boards and 16% at 10,
 * however good the randomness. It only matters when both are plugged into one
 * host, and it is obvious when it happens: POST 12 hex digits to /api/serial
 * and that MAC is used instead, reported as assigned.
 *
 * WHY /serial.txt AND NOT config.ini:
 *
 * The identity must be set before tud_init(), and the config is not loaded
 * until flight_init(), long after. A one-line file can be read with
 * hal_fs_read_file(), which mounts read-only and does NOT format on failure,
 * so a blank board draws a MAC and enumerates instead of waiting out an 8 MB
 * format. It is written once the filesystem is up.
 */
#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_SERIAL_MAX 16

/* Read /serial.txt, or draw a MAC when there is none. Call once, EARLY --
 * before net_mac_init() and tud_init(), because everything below feeds a USB
 * descriptor or the netif address, and both are fixed at enumeration. */
void board_identity_init(void);

/* A MAC drawn this boot and not yet in /serial.txt. board_identity_save()
 * writes it; call it wherever files may be written, until this is false. */
bool board_identity_unsaved(void);
void board_identity_save(void);

/* "rng" or "assigned", for /api/status. */
const char *board_mac_source(void);

/* The MAC as twelve hex digits: the USB serial and the API's "serial". */
const char *board_serial(void);

/* The six MAC bytes, for tud_network_mac_address and the ECM descriptor. */
const uint8_t *board_mac(void);

/* True for a MAC an operator assigned through /api/serial. */
bool board_serial_assigned(void);

/* The flash die's unique id as hex, for the registry: not unique on MK1C. */
const char *board_hw_id(void);

/* Third octet of the board's /24: 192.168.<octet>.1, the MAC's last byte. */
uint8_t board_subnet_octet(void);

#endif
