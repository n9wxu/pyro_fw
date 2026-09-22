/*
 * Binary info for the BOOTLOADER, so `picotool info` identifies the board.
 *
 * This file exists because putting the identification in the application does
 * not work. picotool reads the image at XIP_BASE, and with pico_fota_bootloader
 * that is the bootloader -- the application lives in an A/B slot further up, so
 * its .binary_info section is never scanned. Verified on hardware: an image
 * whose strings are demonstrably in flash still reports "Program Information:
 * none".
 *
 * The bootloader is a target in our own build (FetchContent), so this is added
 * to it with target_sources() rather than by patching the dependency. It emits
 * data only -- no code -- so it cannot change bootloader behaviour.
 *
 * Without this, every board reports the same thing: name pico_fota_bootloader,
 * pico_board pico. Which is exactly the situation where someone flashes MK1C's
 * image onto an MK1A and finds out when GPIO25 -- a pyro bias injector there,
 * the heartbeat LED here -- does something unexpected.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pico/binary_info.h"
#include "board_id.h"

bi_decl(bi_program_description("Pyro bootloader - " BOARD_NAME_STR));
bi_decl(bi_program_url("https://github.com/n9wxu/pyro_fw"));
bi_decl(bi_program_feature("board: " BOARD_NAME_STR " (" BOARD_SHORT_STR ")"));
bi_decl(bi_program_feature("app image: pyro_fw_" BOARD_SHORT_STR ".uf2"));
bi_decl(bi_program_feature("rebuild: cmake -DPYRO_BOARD=" BOARD_SHORT_STR));
