#ifndef PAIR_MODE_H
#define PAIR_MODE_H

#include <stdbool.h>
#include <stdint.h>

void pair_mode_init_from_nvs(void);
bool pair_mode_is_forced(void);
void pair_mode_force_on(void);
void pair_mode_clear_on_success(uint16_t conn_handle);

#endif
