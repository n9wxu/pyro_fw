/*
 * USB descriptors for ECM+RNDIS network device.
 * Adapted from TinyUSB net_lwip_webserver example.
 */
#include "tusb.h"

#define USB_VID   0xCafe
#define USB_PID   0x4002
#define USB_BCD   0x0200

#define STRID_LANGID   0
#define STRID_MFG      1
#define STRID_PRODUCT  2
#define STRID_SERIAL   3
#define STRID_IF       4
#define STRID_MAC      5

enum {
    CONFIG_ID_RNDIS = 0,
    CONFIG_ID_ECM = 1,
    CONFIG_ID_COUNT
};

/* Device descriptor */
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0101,
    .iManufacturer      = STRID_MFG,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = CONFIG_ID_COUNT,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* RNDIS config (Windows) */
#define EPNUM_NET_NOTIF   0x81
#define EPNUM_NET_OUT     0x02
#define EPNUM_NET_IN      0x82

#define MAIN_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN)
static uint8_t const rndis_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(CONFIG_ID_RNDIS + 1, 2, 0, MAIN_CONFIG_TOTAL_LEN, 0, 100),
    TUD_RNDIS_DESCRIPTOR(0, STRID_IF, EPNUM_NET_NOTIF, 8, EPNUM_NET_OUT, EPNUM_NET_IN, CFG_TUD_NET_ENDPOINT_SIZE),
};

/* ECM config (macOS/Linux) */
#define ALT_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_ECM_DESC_LEN)
static uint8_t const ecm_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(CONFIG_ID_ECM + 1, 2, 0, ALT_CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_ECM_DESCRIPTOR(0, STRID_IF, STRID_MAC, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT, EPNUM_NET_IN, CFG_TUD_NET_ENDPOINT_SIZE, CFG_TUD_NET_MTU),
};

static uint8_t const *const config_descriptors[CONFIG_ID_COUNT] = {
    rndis_configuration,
    ecm_configuration,
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    return (index < CONFIG_ID_COUNT) ? config_descriptors[index] : NULL;
}

/* String descriptors */
static char const *string_desc_arr[] = {
    [STRID_LANGID]  = (const char[]){0x09, 0x04},  /* English */
    [STRID_MFG]     = "Pyro",
    [STRID_PRODUCT] = "Pyro MK1B",
    [STRID_SERIAL]  = "000001",
    [STRID_IF]      = "Pyro Network",
    [STRID_MAC]     = "020284006A00",
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
