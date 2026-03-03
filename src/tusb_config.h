#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#include "lwipopts.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#ifndef CFG_TUD_ENABLED
#define CFG_TUD_ENABLED       1
#endif

#ifndef CFG_TUD_MAX_SPEED
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_ECM_RNDIS    1
#define CFG_TUD_NCM          0
#define CFG_TUD_CDC          0
#define CFG_TUD_MSC          0
#define CFG_TUD_HID          0
#define CFG_TUD_MIDI         0
#define CFG_TUD_VENDOR       0

#ifdef __cplusplus
}
#endif

#endif
