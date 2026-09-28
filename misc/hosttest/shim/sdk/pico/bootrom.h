#pragma once
/* Stand-in for the SDK's bootrom. The call handlers.c drops into BOOTSEL with, and the LED
   pin it hands over, which the SDK takes from the board header. */

#include <stdint.h>

#define PICO_DEFAULT_LED_PIN 25

void reset_usb_boot(uint32_t gpio_activity_pin_mask, uint32_t disable_interface_mask);
