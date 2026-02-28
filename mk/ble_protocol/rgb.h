#pragma once

#include <stdbool.h>

void rgb_init(void);
void rgb_set_pairing(bool enabled);
void rgb_set_connected(bool connected);
void rgb_blink_reset(void);
void rgb_notify_pulse(void);
void rgb_blink_pair_success(void);
