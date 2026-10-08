#ifndef LOAD_STATE_H
#define LOAD_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOAD_IDLE = 0,
    LOAD_WORKING = 1,
} load_mode_t;

/* Retained in RTC memory by the application, never written each second to NVS. */
typedef struct {
    load_mode_t mode;
    bool button_was_pressed;
    bool low_current_pending;
    int64_t low_current_since_us;
    float temperature_history[5];
    uint8_t temperature_count;
    uint8_t temperature_next;
} load_state_t;

typedef struct {
    float button_delta_c;
    float idle_current_ma;
    int64_t idle_timeout_us;
    bool require_new_press;
} load_policy_t;

/* Compare the current sample with the median of five preceding valid samples. */
bool load_button_is_pressed(const load_state_t *state, const load_policy_t *policy,
                            float temperature_c);

void load_state_step(load_state_t *state, const load_policy_t *policy,
                     float temperature_c, float current_ma, int64_t now_us);

#endif
