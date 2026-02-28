#include "bsp/board_api.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include <stdio.h>

extern "C" void cdc_app_task(void);
extern "C" void hid_app_task(void);

// I2C defines
// This example will use I2C0 on GPIO8 (SDA) and GPIO9 (SCL) running at 400KHz.
// Pins can be changed, see the GPIO function select table in the datasheet for
// information on GPIO assignments
#define I2C_PORT i2c0
#define I2C_SDA 8
#define I2C_SCL 9

// Data will be copied from src to dst
const char src[] = "Hello, world! (from DMA)";
char dst[count_of(src)];

#include "blink.pio.h"

void blink_pin_forever(PIO pio, uint sm, uint offset, uint pin, uint freq) {
  blink_program_init(pio, sm, offset, pin);
  pio_sm_set_enabled(pio, sm, true);

  printf("Blinking pin %d at %d Hz\n", pin, freq);

  // PIO counter program takes 3 more cycles in total than we pass as
  // input (wait for n + 1; mov; jmp)
  pio->txf[sm] = (125000000 / (2 * freq)) - 3;
}

// UART defines
// By default the stdout UART is `uart0`, so we will use the second one
#define UART_ID uart0
#define BAUD_RATE 115200

// Use pins 4 and 5 for UART1
// Pins can be changed, see the GPIO function select table in the datasheet for
// information on GPIO assignments
#define UART_TX_PIN 0
#define UART_RX_PIN 1

int main() {
  stdio_init_all();
  board_init();

  tud_init(BOARD_TUD_RHPORT);

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

  // // Set up our UART
  // uart_init(UART_ID, BAUD_RATE);
  // // Set the TX and RX pins by using the function select on the GPIO
  // // Set datasheet for more information on function select
  // gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  // gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

  // Use some the various UART functions to send out data
  // In a default system, printf will also output via the default UART

  // Send out a string, with CR/LF conversions
  uart_puts(UART_ID, " Hello, UART!\n");

  // I2C Initialisation. Using it at 400Khz.
  //   i2c_init(I2C_PORT, 400 * 1000);
  //
  //   gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
  //   gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
  //   gpio_pull_up(I2C_SDA);
  //   gpio_pull_up(I2C_SCL);
  // For more examples of I2C use see
  // https://github.com/raspberrypi/pico-examples/tree/master/i2c

  // PIO Blinking example
  PIO pio = pio0;
  uint offset = pio_add_program(pio, &blink_program);
  printf("Loaded program at %d\n", offset);
  blink_pin_forever(pio, 0, offset, 25, 3);

  if (watchdog_caused_reboot()) {
    printf("Rebooted by Watchdog!\n");
  }

  watchdog_enable(100, 1);
  watchdog_update();

  while (true) {
    watchdog_update();
    tud_task();
  }
}
