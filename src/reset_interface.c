/*
 * Vendor reset interface for picotool support.
 * Defers reset to main loop to avoid interrupting I2C.
 */
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "pico/usb_reset_interface.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"

static uint8_t itf_num;

/* Deferred reset flags — checked by main loop */
volatile uint8_t pending_reset = 0;  /* 1=BOOTSEL, 2=flash */

static void resetd_init(void) {}
static void resetd_reset(uint8_t rhport) { (void)rhport; itf_num = 0; }

static uint16_t resetd_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    (void)rhport;
    TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == itf_desc->bInterfaceClass &&
              RESET_INTERFACE_SUBCLASS == itf_desc->bInterfaceSubClass &&
              RESET_INTERFACE_PROTOCOL == itf_desc->bInterfaceProtocol, 0);
    TU_VERIFY(max_len >= sizeof(tusb_desc_interface_t), 0);
    itf_num = itf_desc->bInterfaceNumber;
    return sizeof(tusb_desc_interface_t);
}

static bool resetd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    (void)rhport;
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->wIndex != itf_num) return false;

    if (request->bRequest == RESET_REQUEST_BOOTSEL) {
        pending_reset = 1;
        return true;
    }
    if (request->bRequest == RESET_REQUEST_FLASH) {
        pending_reset = 2;
        return true;
    }
    return false;
}

static bool resetd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)rhport; (void)ep_addr; (void)result; (void)xferred_bytes;
    return true;
}

static usbd_class_driver_t const _resetd_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "RESET",
#endif
    .init            = resetd_init,
    .reset           = resetd_reset,
    .open            = resetd_open,
    .control_xfer_cb = resetd_control_xfer_cb,
    .xfer_cb         = resetd_xfer_cb,
    .sof             = NULL
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &_resetd_driver;
}
