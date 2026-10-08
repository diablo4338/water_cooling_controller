#include "buzzer_logic.h"

#include <math.h>

uint16_t buzzer_step(buzzer_state_t *state, const buzzer_policy_t *policy,
                     bool new_thermal_press, float voltage_v, int64_t now_us) {
    if (!state || !policy) return 0;
    if (!policy->enabled) {
        *state = (buzzer_state_t){0};
        return 0;
    }
    if (state->threshold_mv != policy->threshold_mv) {
        *state = (buzzer_state_t){.threshold_mv = policy->threshold_mv};
    }

    bool low = policy->threshold_mv > 0 && isfinite(voltage_v) &&
               voltage_v * 1000.0f < (float)policy->threshold_mv;
    bool low_alert = false;
    if (!low) {
        state->low_pending = false;
        state->low_alerted = false;
    } else if (!state->low_pending || now_us < state->low_since_us) {
        state->low_pending = true;
        state->low_alerted = false;
        state->low_since_us = now_us;
    } else if (!state->low_alerted &&
               now_us - state->low_since_us > (int64_t)policy->low_delay_s * 1000000LL) {
        state->low_alerted = true;
        low_alert = true;
    }

    if (new_thermal_press) {
        /* A combined signal takes priority when both events occur together. */
        return low ? policy->combined_ms : policy->button_ms;
    }
    return low_alert ? policy->low_ms : 0;
}
