/*
 * Binary info for the BOOTLOADER, so `picotool info` names the board before an
 * image is loaded onto it: an MK1A image on an MK1C drives GPIO25, its LED,
 * which there is BIAS_B, a pyro bias injector.
 *
 * In the bootloader, not the application: picotool reads the image at
 * XIP_BASE, which with pico_fota_bootloader is the bootloader, and never scans
 * the application's A/B slot (on hardware, an application's own strings
 * report "Program Information: none"). Added with target_sources() to the
 * FetchContent target; data only, so it cannot change what the bootloader
 * does.
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
