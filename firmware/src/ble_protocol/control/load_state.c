#include "load_state.h"

#include <math.h>

bool load_button_is_pressed(const load_state_t *state, const load_policy_t *policy,
                            float temperature_c) {
    if (!state || !policy || !isfinite(temperature_c)) return state && state->button_was_pressed;
    if (state->temperature_count < 5) return false;
    float sorted[5];
    for (int i = 0; i < 5; ++i) {
        float value = state->temperature_history[i];
        int j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    return temperature_c - sorted[2] >= policy->button_delta_c;
}

void load_state_step(load_state_t *state, const load_policy_t *policy,
                     float temperature_c, float current_ma, int64_t now_us) {
    bool pressed = load_button_is_pressed(state, policy, temperature_c);
    bool start = pressed && (!policy->require_new_press || !state->button_was_pressed);
    /* A failed ADC read must not manufacture a release or enter the history. */
    if (isfinite(temperature_c)) {
        state->button_was_pressed = pressed;
        state->temperature_history[state->temperature_next] = temperature_c;
        state->temperature_next = (state->temperature_next + 1) % 5;
        if (state->temperature_count < 5) ++state->temperature_count;
    }

    if (state->mode == LOAD_IDLE) {
        state->low_current_pending = false;
        if (start) state->mode = LOAD_WORKING;
        /* This sample was acquired before enabling the load. */
        return;
    }

    /* Only uninterrupted, valid low-current samples qualify for shutdown. */
    if (!isfinite(current_ma) || fabsf(current_ma) >= policy->idle_current_ma) {
        state->low_current_pending = false;
        return;
    }
    if (!state->low_current_pending || now_us < state->low_current_since_us) {
        state->low_current_pending = true;
        state->low_current_since_us = now_us;
    } else if (now_us - state->low_current_since_us >= policy->idle_timeout_us) {
        state->mode = LOAD_IDLE;
        state->low_current_pending = false;
    }
}
