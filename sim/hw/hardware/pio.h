/* Host stand-in for hardware/pio.h. Behavioural: the one program in the
 * tree, boards/mk1c/arm_pump.pio, as its contract -- each pushed word buys
 * (word + 1) toggle cycles, then the state machine stalls on its empty
 * 4-deep FIFO. See boards/mk1c/THEORY_OF_OPERATION.md "Arm pump". */
#ifndef _HARDWARE_PIO_H
#define _HARDWARE_PIO_H
#include "pico/types.h"

typedef struct pio_instance *PIO;
extern PIO const pio0;
extern PIO const pio1;

typedef struct { uint pin_base; uint pin_count; float clkdiv; } pio_sm_config;
typedef struct { uint16_t length; } pio_program_t;

uint pio_add_program(PIO pio, const pio_program_t *program);
int  pio_claim_unused_sm(PIO pio, bool required);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data);
void pio_sm_put(PIO pio, uint sm, uint32_t data);
bool pio_sm_is_tx_fifo_full(PIO pio, uint sm);
void pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config);
void pio_gpio_init(PIO pio, uint pin);
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin_base, uint pin_count, bool is_out);
void sm_config_set_set_pins(pio_sm_config *c, uint set_base, uint set_count);
void sm_config_set_clkdiv(pio_sm_config *c, float div);
#endif
