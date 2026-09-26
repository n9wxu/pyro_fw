#include "fake_sdk.h"

uint32_t fake_now_ms;
bool fake_level[FAKE_PINS];
bool fake_output[FAKE_PINS];
uint32_t fake_writes[FAKE_PINS];
uint32_t fake_rose_ms[FAKE_PINS];
uint8_t fake_func[FAKE_PINS];
uint16_t fake_adc[4];
uint8_t fake_adc_selected;
void (*fake_on_adc_read)(uint8_t channel);
void (*fake_on_put)(uint pin, bool value);
const char *fake_slept;
