#pragma once

#include <stdbool.h>

typedef enum {
    STATUS_LED_MODE_OFF = 0,
    STATUS_LED_MODE_ON,
    STATUS_LED_MODE_BLINK_SLOW,
    STATUS_LED_MODE_BLINK_FAST,
} status_led_mode_t;

void status_led_init(void);
void status_led_task(void *param);
void status_led_blink_twice(void);

#ifdef PAIR_RUN_TESTS
status_led_mode_t status_led_resolve_mode_for_state(bool pairing_active, bool overheat_active, bool error_active);
#endif
