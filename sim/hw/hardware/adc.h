/* Host stand-in for hardware/adc.h, backed by sim/plant/.
 *
 * adc_read() costs one conversion of virtual time; see rp2040_shim.h for
 * why that matters to MK1C's decay probe. */
#ifndef _HARDWARE_ADC_H
#define _HARDWARE_ADC_H
#include "pico/types.h"

void     adc_init(void);
void     adc_gpio_init(uint gpio);
void     adc_select_input(uint input);
uint     adc_get_selected_input(void);
uint16_t adc_read(void);
void     adc_set_temp_sensor_enabled(bool enable);

void     adc_fifo_setup(bool en, bool dreq_en, uint16_t dreq_thresh, bool err_in_fifo, bool byte_shift);
void     adc_fifo_drain(void);
bool     adc_fifo_is_empty(void);
uint16_t adc_fifo_get(void);
void     adc_run(bool run);
void     adc_set_clkdiv(float clkdiv);

/* The firmware passes &adc_hw->fifo to the DMA as a read address. The shim
 * never dereferences it; it is a token the DMA model recognises. */
typedef struct { volatile uint32_t fifo; } adc_hw_t;
extern adc_hw_t *const adc_hw;

#define ADC_CLK_HZ 48000000u
#endif
