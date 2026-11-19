#include "output_limit.h"

#include <math.h>

#include "device_status.h"
#include "esp_log.h"
#include "ina226.h"
#include "params.h"
#include "state.h"

#define CURRENT_SAMPLE_MAX_AGE_US 3500000LL
#define VERIFY_TIMEOUT_US 9000000LL
#define CURRENT_MEASUREMENT_TIMEOUT_US 12000000LL
#define SEARCH_STEP_PERCENT 5.0f

static const char *const TAG_LIMIT = "output_limit";

typedef enum {
    LIMIT_EVENT_CURRENT_EXCEEDED,
    LIMIT_EVENT_CURRENT_WITHOUT_LOAD,
    LIMIT_EVENT_CONFIG_READY,
    LIMIT_EVENT_MEASUREMENT_CANCELLED,
    LIMIT_EVENT_MEASURED,
    LIMIT_EVENT_ADVANCE_PROBE,
    LIMIT_EVENT_SAFE,
    LIMIT_EVENT_MINIMUM_APPLIED,
    LIMIT_EVENT_SENSOR_RECOVERED,
    LIMIT_EVENT_OVER_LIMIT,
    LIMIT_EVENT_SENSOR_UNAVAILABLE,
    LIMIT_EVENT_VERIFICATION_TIMED_OUT,
    LIMIT_EVENT_STOP_REQUESTED,
    LIMIT_EVENT_RESET,
} output_limit_event_t;

typedef struct {
    output_limit_state_t state;
    float cap;
    int64_t entered_us;
    output_limit_state_t prior_state;
    float prior_cap;
    float measurement_cap;
    float adjustment_minimum;
    uint8_t adjustment_maximum;
    output_limit_mode_t adjustment_mode;
    bool alert_without_load;
    int64_t output_changed_us;
    float applied_percent;
    uint8_t applied_control_type;
} output_limit_context_t;

static output_limit_context_t g_limit = {
    .cap = 100.0f, .applied_control_type = UINT8_MAX,
};

static output_limit_status_t g_status = {
    .configured = 100, .effective = 100, .confirmed_effective = 100,
    .temporary_minimum = OUTPUT_LIMIT_NO_TEMPORARY_MINIMUM,
};
static bool g_reset_allowed;
static bool g_stop_requested;
static bool g_config_write_in_progress;

static const char *const state_names[] = {
    [OUTPUT_LIMIT_NORMAL] = "normal",
    [OUTPUT_LIMIT_MEASURE] = "measure",
    [OUTPUT_LIMIT_VERIFY_FORMULA] = "verify_formula",
    [OUTPUT_LIMIT_VERIFY_STEP] = "verify_step",
    [OUTPUT_LIMIT_SEARCH_UP] = "search_up",
    [OUTPUT_LIMIT_LIMITED] = "limited",
    [OUTPUT_LIMIT_MINIMUM] = "minimum",
    [OUTPUT_LIMIT_SENSOR_ERROR] = "sensor_error",
    [OUTPUT_LIMIT_FAULT] = "fault",
    [OUTPUT_LIMIT_VERIFY_TIMEOUT] = "verify_timeout",
    [OUTPUT_LIMIT_WAIT_CONFIG] = "wait_config",
};

static const char *const event_names[] = {
    [LIMIT_EVENT_CURRENT_EXCEEDED] = "current_exceeded",
    [LIMIT_EVENT_CURRENT_WITHOUT_LOAD] = "current_without_load",
    [LIMIT_EVENT_CONFIG_READY] = "config_ready",
    [LIMIT_EVENT_MEASUREMENT_CANCELLED] = "current_within_limit_before_probe",
    [LIMIT_EVENT_MEASURED] = "current_measured",
    [LIMIT_EVENT_ADVANCE_PROBE] = "advance_probe",
    [LIMIT_EVENT_SAFE] = "current_within_limit",
    [LIMIT_EVENT_MINIMUM_APPLIED] = "fallback_output_applied",
    [LIMIT_EVENT_SENSOR_RECOVERED] = "sensor_recovered",
    [LIMIT_EVENT_OVER_LIMIT] = "current_still_exceeded",
    [LIMIT_EVENT_SENSOR_UNAVAILABLE] = "sensor_unavailable",
    [LIMIT_EVENT_VERIFICATION_TIMED_OUT] = "verification_timed_out",
    [LIMIT_EVENT_STOP_REQUESTED] = "stop_requested",
    [LIMIT_EVENT_RESET] = "reset",
};

static bool output_limit_adjusting(output_limit_state_t state) {
    return state == OUTPUT_LIMIT_MEASURE || state == OUTPUT_LIMIT_VERIFY_FORMULA ||
           state == OUTPUT_LIMIT_SEARCH_UP || state == OUTPUT_LIMIT_VERIFY_STEP;
}

bool output_limit_is_adjusting(output_limit_state_t state) {
    return output_limit_adjusting(state) || state == OUTPUT_LIMIT_SENSOR_ERROR ||
           state == OUTPUT_LIMIT_WAIT_CONFIG;
}

static bool output_limit_alarming(output_limit_state_t state) {
    return state == OUTPUT_LIMIT_MINIMUM ||
           state == OUTPUT_LIMIT_SENSOR_ERROR || state == OUTPUT_LIMIT_VERIFY_TIMEOUT ||
           state == OUTPUT_LIMIT_FAULT;
}

static void output_limit_transition(output_limit_state_t state, float cap, int64_t now) {
    state_lock();
    g_limit.state = state;
    state_unlock();
    g_limit.cap = cap;
    g_limit.entered_us = now;
}

static float output_limit_probe_maximum(void) {
    return floorf(fminf(g_limit.measurement_cap, fminf(g_limit.prior_cap, g_limit.adjustment_maximum)));
}

static void output_limit_probe(output_limit_state_t state, float candidate, int64_t now) {
    float minimum = g_limit.adjustment_mode == OUTPUT_LIMIT_MODE_SERVICE ? g_limit.adjustment_minimum : 0.0f;
    float cap = fmaxf(0.0f, fminf(output_limit_probe_maximum(), floorf(candidate)));
    ESP_LOGW(TAG_LIMIT, "probe %s candidate=%.2f%% cap=%.0f%% minimum=%.0f%% below_minimum=%u",
             state_names[state], candidate, cap, minimum, (unsigned)(cap < minimum));
    if (cap < minimum) {
        output_limit_transition(OUTPUT_LIMIT_FAULT, 0.0f, now);
    } else {
        output_limit_transition(state, cap, now);
    }
}

static void output_limit_dispatch(output_limit_event_t event, float actual, float minimum,
                                  output_limit_mode_t mode, const ina226_snapshot_t *sample, int64_t now) {
    output_limit_state_t previous = g_limit.state;
    int64_t elapsed_us = now - g_limit.entered_us;
    switch (event) {
        case LIMIT_EVENT_RESET:
            g_limit.alert_without_load = false;
            output_limit_transition(OUTPUT_LIMIT_NORMAL, 100.0f, now);
            break;
        case LIMIT_EVENT_STOP_REQUESTED:
            if (output_limit_is_adjusting(g_limit.state)) {
                output_limit_transition(g_limit.prior_state, g_limit.prior_cap, now);
            }
            break;
        case LIMIT_EVENT_CURRENT_WITHOUT_LOAD:
            g_limit.alert_without_load = true;
            output_limit_transition(OUTPUT_LIMIT_FAULT, 0.0f, now);
            break;
        case LIMIT_EVENT_CURRENT_EXCEEDED:
            g_limit.prior_state = g_limit.state;
            g_limit.prior_cap = g_limit.cap;
            g_limit.measurement_cap = actual;
            g_limit.adjustment_mode = mode;
            g_limit.adjustment_maximum = params_read_config().maximum;
            output_limit_transition(OUTPUT_LIMIT_WAIT_CONFIG, g_limit.cap, now);
            break;
        case LIMIT_EVENT_CONFIG_READY: {
            params_config_t config = params_read_config();
            g_limit.adjustment_maximum = config.maximum;
            g_limit.adjustment_minimum = (float)config.params.fan_min_speed;
            output_limit_transition(OUTPUT_LIMIT_MEASURE, g_limit.cap, now);
            break;
        }
        case LIMIT_EVENT_MEASUREMENT_CANCELLED:
        case LIMIT_EVENT_SENSOR_RECOVERED:
            output_limit_transition(g_limit.prior_state, g_limit.prior_cap, now);
            break;
        case LIMIT_EVENT_MEASURED: {
            float candidate = actual * (float)INA226_OVERCURRENT_THRESHOLD_MA / sample->current_ma;
            output_limit_probe(OUTPUT_LIMIT_VERIFY_FORMULA, fminf(actual, candidate), now);
            break;
        }
        case LIMIT_EVENT_ADVANCE_PROBE:
            output_limit_probe(OUTPUT_LIMIT_SEARCH_UP, actual + SEARCH_STEP_PERCENT, now);
            break;
        case LIMIT_EVENT_SAFE:
            output_limit_transition(OUTPUT_LIMIT_LIMITED, fminf(floorf(actual), g_limit.cap), now);
            break;
        case LIMIT_EVENT_MINIMUM_APPLIED:
            output_limit_transition(OUTPUT_LIMIT_MINIMUM, g_limit.cap, now);
            break;
        case LIMIT_EVENT_OVER_LIMIT:
            if (g_limit.state == OUTPUT_LIMIT_VERIFY_FORMULA || g_limit.state == OUTPUT_LIMIT_SEARCH_UP) {
                output_limit_probe(OUTPUT_LIMIT_VERIFY_STEP, actual - SEARCH_STEP_PERCENT, now);
            } else {
                output_limit_transition(OUTPUT_LIMIT_FAULT, 0.0f, now);
            }
            break;
        case LIMIT_EVENT_SENSOR_UNAVAILABLE:
        case LIMIT_EVENT_VERIFICATION_TIMED_OUT:
            if (g_limit.state == OUTPUT_LIMIT_WAIT_CONFIG) {
                output_limit_transition(OUTPUT_LIMIT_FAULT, 0.0f, now);
                break;
            }
            if (output_limit_adjusting(g_limit.state)) minimum = g_limit.adjustment_minimum;
            output_limit_transition(
                event == LIMIT_EVENT_SENSOR_UNAVAILABLE ? OUTPUT_LIMIT_SENSOR_ERROR : OUTPUT_LIMIT_VERIFY_TIMEOUT,
                fminf(g_limit.cap, fminf(actual, minimum)), now);

            break;
    }
    if (previous != g_limit.state || event == LIMIT_EVENT_ADVANCE_PROBE) {
        ESP_LOGW(TAG_LIMIT, "%s -> %s event=%s elapsed=%lldms output=%.0f%% cap=%.0f%% "
                 "current=%.2fmA sample=%lu window_after=%lldms applied_at=%lldms valid=%u threshold=%umA",
                 state_names[previous], state_names[g_limit.state], event_names[event],
                 (long long)(elapsed_us / 1000), actual, g_limit.cap,
                 sample->current_ma, (unsigned long)sample->sequence,
                 (long long)(sample->conversion_started_after_us / 1000),
                 (long long)(g_limit.output_changed_us / 1000), (unsigned)sample->sample_valid,
                 (unsigned)INA226_OVERCURRENT_THRESHOLD_MA);
    }
}

static bool output_limit_reset_allowed(output_limit_state_t state, bool healthy) {
    return healthy && (state == OUTPUT_LIMIT_NORMAL || state == OUTPUT_LIMIT_LIMITED ||
                       state == OUTPUT_LIMIT_MINIMUM || state == OUTPUT_LIMIT_SENSOR_ERROR ||
                       state == OUTPUT_LIMIT_VERIFY_TIMEOUT || state == OUTPUT_LIMIT_FAULT);
}

output_limit_command_t output_limit_step(float requested, float actual, uint8_t control_type,
                                         uint8_t maximum, float minimum, output_limit_mode_t mode, int64_t now) {
    ina226_snapshot_t sample = ina226_get_snapshot();
    bool sensor_offline = sample.state == INA226_OFFLINE;
    bool fresh = sample.sample_valid && !sensor_offline && isfinite(sample.current_ma) &&
                 now >= sample.sampled_us && now - sample.sampled_us <= CURRENT_SAMPLE_MAX_AGE_US;
    bool measured_output = fresh && g_limit.applied_control_type != UINT8_MAX &&
                           actual == g_limit.applied_percent && sample.sampled_us > g_limit.output_changed_us &&
                           sample.conversion_started_after_us >= g_limit.output_changed_us;
    bool over_limit = measured_output && sample.current_ma > (float)INA226_OVERCURRENT_THRESHOLD_MA;
    bool healthy = measured_output && !over_limit;
    bool stop = g_stop_requested;
    g_stop_requested = false;
    if (stop) output_limit_dispatch(LIMIT_EVENT_STOP_REQUESTED, actual, minimum, mode, &sample, now);

    state_lock();
    bool reset = g_status.reset_result == OUTPUT_LIMIT_RESET_PENDING;
    state_unlock();
    output_limit_reset_result_t reset_result = OUTPUT_LIMIT_RESET_NONE;
    if (reset) {
        bool accepted = output_limit_reset_allowed(g_limit.state, healthy);
        reset_result = accepted ? OUTPUT_LIMIT_RESET_APPLIED : OUTPUT_LIMIT_RESET_REJECTED;
        if (accepted && g_limit.state != OUTPUT_LIMIT_NORMAL) {
            output_limit_dispatch(LIMIT_EVENT_RESET, actual, minimum, mode, &sample, now);
        }
    }

    if (actual == 0.0f && over_limit && g_limit.state != OUTPUT_LIMIT_FAULT) {
        output_limit_dispatch(LIMIT_EVENT_CURRENT_WITHOUT_LOAD, actual, minimum, mode, &sample, now);
    }
    if (!stop && maximum > 0 && actual > 0.0f && over_limit &&
        (g_limit.state == OUTPUT_LIMIT_NORMAL || g_limit.state == OUTPUT_LIMIT_LIMITED)) {
        output_limit_dispatch(LIMIT_EVENT_CURRENT_EXCEEDED, actual, minimum, mode, &sample, now);
    }
    if (g_limit.state == OUTPUT_LIMIT_WAIT_CONFIG) {
        state_lock();
        bool config_ready = !g_config_write_in_progress;
        state_unlock();
        if (healthy) {
            output_limit_dispatch(LIMIT_EVENT_MEASUREMENT_CANCELLED, actual, minimum, mode, &sample, now);
        } else if (config_ready) {
            output_limit_dispatch(LIMIT_EVENT_CONFIG_READY, actual, minimum, mode, &sample, now);
        } else if (now - g_limit.entered_us >= VERIFY_TIMEOUT_US) {
            output_limit_dispatch(LIMIT_EVENT_VERIFICATION_TIMED_OUT, actual, minimum, mode, &sample, now);
        }
    }
    if (output_limit_adjusting(g_limit.state) && sensor_offline) {
        output_limit_dispatch(LIMIT_EVENT_SENSOR_UNAVAILABLE, actual, minimum, mode, &sample, now);
    }

    float verifying_percent = fminf(maximum, fminf(g_limit.adjustment_maximum, g_limit.cap));
    int64_t probe_started_us = g_limit.output_changed_us > g_limit.entered_us
                               ? g_limit.output_changed_us : g_limit.entered_us;
    bool probe_applied = actual == verifying_percent && actual == g_limit.applied_percent;
    bool probe_ready = probe_applied && measured_output && sample.sampled_us > probe_started_us &&
                       sample.conversion_started_after_us >= probe_started_us;
    if (!stop && maximum > 0) {
        switch (g_limit.state) {
            case OUTPUT_LIMIT_MEASURE:
                if (over_limit) {
                    output_limit_dispatch(LIMIT_EVENT_MEASURED, actual, minimum, mode, &sample, now);
                } else if (healthy) {
                    output_limit_dispatch(LIMIT_EVENT_MEASUREMENT_CANCELLED, actual, minimum, mode, &sample, now);
                } else if (now - g_limit.entered_us >= CURRENT_MEASUREMENT_TIMEOUT_US) {
                    output_limit_dispatch(LIMIT_EVENT_VERIFICATION_TIMED_OUT, actual, minimum, mode, &sample, now);
                }
                break;
            case OUTPUT_LIMIT_VERIFY_FORMULA:
            case OUTPUT_LIMIT_SEARCH_UP:
            case OUTPUT_LIMIT_VERIFY_STEP:
                if (probe_ready) {
                    bool advance = actual > 0.0f && g_limit.state != OUTPUT_LIMIT_VERIFY_STEP &&
                                   actual < fminf(maximum, output_limit_probe_maximum());
                    output_limit_event_t event = over_limit ? LIMIT_EVENT_OVER_LIMIT :
                                                 advance ? LIMIT_EVENT_ADVANCE_PROBE : LIMIT_EVENT_SAFE;
                    output_limit_dispatch(event, actual, minimum, mode, &sample, now);
                } else if (now - g_limit.entered_us >= VERIFY_TIMEOUT_US) {
                    output_limit_dispatch(LIMIT_EVENT_VERIFICATION_TIMED_OUT, actual, minimum, mode, &sample, now);
                }
                break;
            case OUTPUT_LIMIT_SENSOR_ERROR:
                if (probe_applied) {
                    output_limit_dispatch(over_limit ? LIMIT_EVENT_OVER_LIMIT : LIMIT_EVENT_MINIMUM_APPLIED,
                                          actual, minimum, mode, &sample, now);
                } else if (now - g_limit.entered_us >= VERIFY_TIMEOUT_US) {
                    output_limit_dispatch(LIMIT_EVENT_VERIFICATION_TIMED_OUT, actual, minimum, mode, &sample, now);
                }
                break;
            case OUTPUT_LIMIT_MINIMUM:
                if (over_limit) {
                    output_limit_dispatch(LIMIT_EVENT_OVER_LIMIT, actual, minimum, mode, &sample, now);
                } else if (healthy) {
                    output_limit_dispatch(LIMIT_EVENT_SENSOR_RECOVERED, actual, minimum, mode, &sample, now);
                }
                break;
            case OUTPUT_LIMIT_VERIFY_TIMEOUT:
                if (over_limit) {
                    output_limit_dispatch(LIMIT_EVENT_OVER_LIMIT, actual, minimum, mode, &sample, now);
                }
                break;
            case OUTPUT_LIMIT_NORMAL:
            case OUTPUT_LIMIT_LIMITED:
            case OUTPUT_LIMIT_START_BLOCKED:
            case OUTPUT_LIMIT_CONFIRMING:
            case OUTPUT_LIMIT_VERIFY_MINIMUM:
            case OUTPUT_LIMIT_WAIT_CONFIG:
            case OUTPUT_LIMIT_FAULT:
                break;
        }
    }

    bool adjusting = output_limit_is_adjusting(g_limit.state);
    uint8_t command_maximum = adjusting && g_limit.adjustment_maximum < maximum ? g_limit.adjustment_maximum : maximum;
    float effective = fminf(command_maximum, g_limit.cap);
    if (g_limit.state == OUTPUT_LIMIT_MEASURE || g_limit.state == OUTPUT_LIMIT_WAIT_CONFIG) {
        effective = fminf(effective, g_limit.measurement_cap);
    }
    // During adjustment the protection owns the applied command.
    output_limit_command_t command = {
        .percent = stop ? 0.0f : adjusting ? effective : fmaxf(0.0f, fminf(requested, effective)),
        .control_type = adjusting && g_limit.applied_control_type != UINT8_MAX ? g_limit.applied_control_type : control_type,
    };

    output_limit_state_t state = g_limit.state;
    float confirmed = adjusting ? fminf(maximum, g_limit.prior_cap) : effective;
    state_lock();
    g_status.state = state;
    g_status.configured = maximum;
    g_status.effective = (uint8_t)floorf(effective);
    g_status.confirmed_effective = (uint8_t)floorf(confirmed);
    g_status.temporary_minimum = state == OUTPUT_LIMIT_LIMITED && mode == OUTPUT_LIMIT_MODE_TEMPERATURE
                                 ? (uint8_t)fminf(minimum, confirmed) : OUTPUT_LIMIT_NO_TEMPORARY_MINIMUM;
    g_status.alert_without_load = g_limit.alert_without_load;
    g_status.verified_output = healthy &&
                               command.percent > 0.0f && command.percent == actual &&
                               command.percent == g_limit.applied_percent &&
                               command.control_type == g_limit.applied_control_type;
    if (reset) g_status.reset_result = reset_result;
    g_reset_allowed = output_limit_reset_allowed(state, healthy);
    state_unlock();

    const device_error_mask_t owned_errors = DEVICE_ERROR_OVERCURRENT | DEVICE_ERROR_OUTPUT_ALARM |
                                             DEVICE_ERROR_OUTPUT_SENSOR | DEVICE_ERROR_OUTPUT_VERIFY |
                                             DEVICE_ERROR_ALERT_WITHOUT_LOAD;
    device_error_mask_t active_errors = DEVICE_ERROR_NONE;
    if (state != OUTPUT_LIMIT_NORMAL && state != OUTPUT_LIMIT_LIMITED) active_errors |= DEVICE_ERROR_OVERCURRENT;
    if (output_limit_alarming(state)) active_errors |= DEVICE_ERROR_OUTPUT_ALARM;
    if (state == OUTPUT_LIMIT_SENSOR_ERROR || state == OUTPUT_LIMIT_MINIMUM) {
        active_errors |= DEVICE_ERROR_OUTPUT_SENSOR;
    }
    if (state == OUTPUT_LIMIT_VERIFY_TIMEOUT) active_errors |= DEVICE_ERROR_OUTPUT_VERIFY;
    if (g_limit.alert_without_load) active_errors |= DEVICE_ERROR_ALERT_WITHOUT_LOAD;
    device_status_update_error_flags(owned_errors, active_errors);
    return command;
}

void output_limit_output_applied(float percent, uint8_t control_type, int64_t now) {
    if (percent == g_limit.applied_percent && control_type == g_limit.applied_control_type) return;
    g_limit.applied_percent = percent;
    g_limit.applied_control_type = control_type;
    g_limit.output_changed_us = now;
    if (output_limit_is_adjusting(g_limit.state)) {
        ESP_LOGW(TAG_LIMIT, "%s applied output=%.0f%% control=%u",
                 state_names[g_limit.state], percent, (unsigned)control_type);
    }
}

output_limit_status_t output_limit_get_status(void) {
    state_lock();
    output_limit_status_t status = g_status;
    state_unlock();
    return status;
}

bool output_limit_settings_locked(void) {
    state_lock();
    bool locked = output_limit_is_adjusting(g_limit.state);
    state_unlock();
    return locked;
}

bool output_limit_begin_config_write(void) {
    state_lock();
    bool allowed = !g_config_write_in_progress && !output_limit_is_adjusting(g_limit.state);
    if (allowed) g_config_write_in_progress = true;
    state_unlock();
    return allowed;
}

void output_limit_end_config_write(void) {
    state_lock();
    g_config_write_in_progress = false;
    state_unlock();
}

void output_limit_request_stop(void) {
    g_stop_requested = true;
}

bool output_limit_request_reset(void) {
    state_lock();
    bool accepted = g_reset_allowed && g_status.reset_result != OUTPUT_LIMIT_RESET_PENDING;
    if (accepted) {
        g_status.reset_sequence++;
        g_status.reset_result = OUTPUT_LIMIT_RESET_PENDING;
    }
    state_unlock();
    return accepted;
}
