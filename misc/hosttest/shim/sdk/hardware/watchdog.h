#pragma once
/* Stand-in for the SDK's watchdog. Only the scratch registers, which is how
   config_enable_hotkey_handler in handlers.c leaves word for the next boot to come up in
   config mode. The registers behind it are defined by the one test that links handlers.c. */

#include <stdint.h>

typedef struct {
    volatile uint32_t scratch[8];
} watchdog_hw_t;

extern watchdog_hw_t watchdog_hw_stand_in;

#define watchdog_hw (&watchdog_hw_stand_in)
