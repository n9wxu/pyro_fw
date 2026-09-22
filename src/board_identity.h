/*
 * Board identity: MAC, serial number, USB serial string and IP subnet.
 *
 * Every board used to ship the same MAC (02:02:84:00:6A:00), the same USB
 * serial ("000001") and the same address (192.168.7.1). Three boards on one
 * host meant one usable board: the others enumerated and were ignored, and
 * `picotool --ser` could not target any of them.
 *
 * DERIVED, NOT ASSIGNED. Everything here comes from the flash chip's 64-bit
 * unique id, so there is no allocation to perform and no registry to keep in
 * step with the hardware. The RP2040 itself has no factory MAC and no on-die
 * GUID -- pico_get_unique_board_id() reads the QSPI flash over JEDEC 0x4B, and
 * the flash vendor guarantees uniqueness. Two consequences worth knowing:
 *
 *   - the identity belongs to the flash chip, not the PCB. Reflow a new flash
 *     and the board's identity changes; move that flash to another PCB and the
 *     identity goes with it.
 *   - there is no OUI, so the MAC must be locally administered (0x02 first
 *     octet). That is correct here and costs nothing.
 *
 * THE ONE PLACE IT IS PROBABILISTIC is the subnet. The octet is 8 bits, so by
 * the birthday bound two boards share a subnet with about 4% probability at 5
 * boards and 16% at 10. It only matters when both are plugged into the same
 * host at once, and it is obvious when it happens. /serial.txt is the escape
 * hatch: write 12 hex digits there (POST /api/serial) and that MAC is used
 * instead. It is an override for a collision, not a provisioning step -- a
 * board with no file is fully functional and unique in every other respect.
 *
 * WHY /serial.txt AND NOT config.ini:
 *
 * The identity must be set before tud_init(), and the config is not loaded
 * until flight_init(), long after. A one-line file can be read with
 * hal_fs_read_file(), which mounts read-only and -- importantly -- does NOT
 * format on failure. So a board with an override gets it before USB
 * enumerates, and a board without one falls straight through to the derived
 * identity instead of waiting out an 8 MB format.
 */
#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_SERIAL_MAX 16

/* Read /serial.txt if there is one. Call once, EARLY -- before net_mac_init()
 * and tud_init(), because everything below feeds a USB descriptor or the
 * netif address, and both are fixed at enumeration. */
void board_identity_init(void);

/* Printable identity for the API and the web pages: the assigned MAC as hex,
 * or the hardware id when unassigned. */
const char *board_serial(void);

/* The six MAC bytes, for tud_network_mac_address and the ECM descriptor. */
const uint8_t *board_mac(void);

/* False when running on the hardware-id fallback, which is what a
 * provisioning tool looks for. Reported by /api/status. */
bool board_serial_assigned(void);

/* The flash die's unique id as hex. Stable across reflash and reformat, so a
 * tool can recognise a board whose filesystem was wiped and give it back the
 * same serial rather than burning another subnet. */
const char *board_hw_id(void);

/* Third octet of the board's /24: 192.168.<octet>.1. Taken from the last
 * three characters of an assigned serial, or derived from the hardware id
 * when unassigned. */
uint8_t board_subnet_octet(void);

#endif
