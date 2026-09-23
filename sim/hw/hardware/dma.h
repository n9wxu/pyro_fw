/* Host stand-in for hardware/dma.h, backed by sim/plant/.
 *
 * Only the one pattern MK1C uses is modelled: a 16-bit, write-incrementing
 * transfer paced by DREQ_ADC. See rp2040_shim.h. */
#ifndef _HARDWARE_DMA_H
#define _HARDWARE_DMA_H
#include "pico/types.h"

typedef enum { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 } enum_dma_channel_transfer_size;
#define DREQ_ADC 36

typedef struct {
    bool     read_incr;
    bool     write_incr;
    uint     dreq;
    int      size;
} dma_channel_config;

int  dma_claim_unused_channel(bool required);
void dma_channel_unclaim(uint channel);
dma_channel_config dma_channel_get_default_config(uint channel);
void channel_config_set_transfer_data_size(dma_channel_config *c, enum_dma_channel_transfer_size size);
void channel_config_set_read_increment(dma_channel_config *c, bool incr);
void channel_config_set_write_increment(dma_channel_config *c, bool incr);
void channel_config_set_dreq(dma_channel_config *c, uint dreq);
void dma_channel_configure(uint channel, dma_channel_config *c, volatile void *write_addr,
                           const volatile void *read_addr, uint transfer_count, bool trigger);
void dma_channel_wait_for_finish_blocking(uint channel);
bool dma_channel_is_busy(uint channel);
#endif
