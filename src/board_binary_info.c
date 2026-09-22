/*
 * Binary info for the APPLICATION image.
 *
 * NOT what picotool reads. picotool scans the image at XIP_BASE, which with
 * pico_fota_bootloader is the bootloader; the application lives in an A/B slot
 * further up and this section is never reached. Verified on hardware -- an app
 * whose strings were provably in flash still reported "Program Information:
 * none". Board identification therefore lives in src/bootloader_binary_info.c.
 *
 * Kept anyway, for the one thing the bootloader cannot carry: the version.
 * scripts/gen_version.sh rewrites src/version.h on every local build, and the
 * bootloader is reflashed far less often than the application, so a version
 * baked into it would go stale and lie. Here it is always the version actually
 * running, and it is reachable the ways a running board is reachable --
 * /api/status and the telemetry UART -- plus `strings` on the image when
 * working out what a .uf2 on disk actually is.
 *
 * pico_set_program_version() in CMake cannot do this: the number does not
 * exist at configure time. That line carried a hardcoded "0.1" that had never
 * matched a shipped build, and has been removed.
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

/* A per-unit serial -- to tell two boards of the SAME model apart -- would be
 * bi_ptr_string(), which picotool can rewrite in place with
 * `picotool config -s <key> <value>` and no rebuild. Not used here: the macro
 * expands to a variable declaration plus an info struct, so it does not sit
 * inside bi_decl() the way every other entry does, and SDK 2.2.0 ships no
 * usage example to copy. Worth revisiting with a working reference rather
 * than a guess, since the entries above identify the MODEL and this is the
 * one question a pin map cannot answer. */
