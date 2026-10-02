/*
 * Binary info for the APPLICATION image: not what picotool reads, which is the
 * bootloader's (src/bootloader_binary_info.c), but the one place the running
 * version is baked in -- the bootloader is reflashed far less often, and the
 * number does not exist at configure time for pico_set_program_version().
 * `strings` on a .uf2 finds it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pico/binary_info.h"
#include "board_id.h"
#include "version.h"

/* Two spellings on purpose. The display name is what a person reads; the
 * short name is what scripts match on, and it is the same token the build
 * takes as -DPYRO_BOARD=<short>, so "which image do I rebuild" has an answer
 * that does not involve a lookup table. */
bi_decl(bi_program_description("Pyro flight computer - " BOARD_NAME_STR));
bi_decl(bi_program_version_string(FW_VERSION));
bi_decl(bi_program_build_date_string(FW_BUILD_DATE));
bi_decl(bi_program_url("https://github.com/n9wxu/pyro_fw"));
bi_decl(bi_program_feature("board: " BOARD_NAME_STR " (" BOARD_SHORT_STR ")"));
bi_decl(bi_program_feature("rebuild: cmake -DPYRO_BOARD=" BOARD_SHORT_STR));
