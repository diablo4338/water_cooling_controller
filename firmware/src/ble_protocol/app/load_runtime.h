#ifndef LOAD_RUNTIME_H
#define LOAD_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "power_manager.h"

typedef struct {
    uint8_t load_state;
    uint8_t button_pressed;
    float temperature_c;
    float voltage_v;
    float current_ma;
} load_runtime_snapshot_t;

/* Restore retained outputs, configure board GPIOs, initialize the event bus,
 * action executor and all three runtime FSMs.  Does not start BLE. */
void load_runtime_init(void);

/* Serialized event dispatcher and scheduler.  Runs forever (or until Deep
 * Sleep is entered by the action executor). */
void load_runtime_task(void *arg);

void load_runtime_get_snapshot(load_runtime_snapshot_t *out);

/* Power Manager inputs, read by the action executor under the dispatch lock. */
void load_runtime_get_power_snapshot(power_snapshot_t *out);

/* Called after an attempted Deep Sleep command returns to the executor. */
void load_runtime_sleep_attempt_finished(void);

/* Final Deep Sleep preparation.  Returns false if sleep must not be entered. */
bool load_runtime_prepare_sleep(void);

#endif
