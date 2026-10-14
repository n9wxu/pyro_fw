/*
 * Board identity: MAC, serial number, USB serial string and IP subnet,
 * derived from the flash chip's whole unique ID [DD-099], or assigned by an
 * operator into /serial.txt.
 *
 * /serial.txt and not config.ini: the identity feeds USB descriptors fixed at
 * tud_init(), long before flight_init() loads the config, and a one-line file
 * can be read before the mount -- read-only, never formatting a blank board.
 *
 * The subnet is the MAC's last byte, so two of n boards share one with the
 * birthday probability over 256 (4% at 5 boards); POST /api/serial assigns a
 * MAC when two meet on one host.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_SERIAL_MAX 16

/* The MAC /serial.txt assigns, or the flash chip's. Call once, after
 * flash_chip_read() and before net_mac_init() and tud_init(): both are
 * fixed at enumeration. Writes nothing. */
void board_identity_init(void);

/* "flash" or "assigned", for /api/status. */
const char *board_mac_source(void);

/* The MAC as twelve hex digits: the USB serial and the API's "serial". */
const char *board_serial(void);

/* The six MAC bytes, for tud_network_mac_address and the ECM descriptor. */
const uint8_t *board_mac(void);

/* True for a MAC an operator assigned through /api/serial. */
bool board_serial_assigned(void);

/* The SDK's 8 bytes of the flash chip's unique ID, as hex: two MK1Cs share
 * them. flash_chip_uid_hex() is the whole ID. */
const char *board_hw_id(void);

/* Third octet of the board's /24: 192.168.<octet>.1, the MAC's last byte. */
uint8_t board_subnet_octet(void);

#endif
