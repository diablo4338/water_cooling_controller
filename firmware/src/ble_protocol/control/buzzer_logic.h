#ifndef BUZZER_LOGIC_H
#define BUZZER_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Stored in RTC memory together with the load state. */
typedef struct {
    bool low_pending;
    bool low_alerted;
    int64_t low_since_us;
    uint16_t threshold_mv;
} buzzer_state_t;

typedef struct {
    bool enabled;
    uint16_t threshold_mv;
    uint16_t low_delay_s;
    uint16_t button_ms;
    uint16_t low_ms;
    uint16_t combined_ms;
} buzzer_policy_t;

/* Return the signal duration in milliseconds, or zero when no signal is due. */
uint16_t buzzer_step(buzzer_state_t *state, const buzzer_policy_t *policy,
                     bool new_thermal_press, float voltage_v, int64_t now_us);

#endif
