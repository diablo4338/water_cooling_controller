#ifndef LOAD_RUNTIME_H
#define LOAD_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t load_state;
    uint8_t button_pressed;
    float temperature_c;
    float voltage_v;
    float current_ma;
} load_runtime_snapshot_t;

/* Restore retained outputs before initializing any other hardware. */
void load_runtime_init(void);
void load_runtime_task(void *arg);
void load_runtime_cycle(void);
void load_runtime_process_pair_button(void);
void load_runtime_sleep(void);
void load_runtime_open_window(void);
void load_runtime_close_window(void);
int32_t load_runtime_window_remaining_ms(void);
bool load_runtime_connection_blocks_sleep(bool connected);
void load_runtime_get_snapshot(load_runtime_snapshot_t *out);

#endif
