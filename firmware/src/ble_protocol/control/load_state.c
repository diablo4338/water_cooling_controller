#include "load_state.h"

#include <math.h>

void load_state_step(load_state_t *state, const load_policy_t *policy,
                     float temperature_c, float current_ma, int64_t now_us) {
    bool pressed = isfinite(temperature_c) && temperature_c > policy->button_temperature_c;
    bool start = pressed && (!policy->require_new_press || !state->button_was_pressed);
    /* A failed ADC read must not manufacture a release followed by a new press. */
    if (isfinite(temperature_c)) state->button_was_pressed = pressed;

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
