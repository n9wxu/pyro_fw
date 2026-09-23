/* Host stand-in for hardware/gpio.h, backed by sim/plant/. */
#ifndef _HARDWARE_GPIO_H
#define _HARDWARE_GPIO_H
#include "pico/types.h"

enum gpio_function { GPIO_FUNC_SIO = 5, GPIO_FUNC_PIO0 = 6, GPIO_FUNC_NULL = 0x1f };
#define GPIO_OUT true
#define GPIO_IN  false

void gpio_init(uint gpio);
void gpio_set_dir(uint gpio, bool out);
void gpio_put(uint gpio, bool value);
bool gpio_get(uint gpio);
void gpio_pull_up(uint gpio);
void gpio_pull_down(uint gpio);
void gpio_disable_pulls(uint gpio);
void gpio_set_function(uint gpio, enum gpio_function fn);
#endif
