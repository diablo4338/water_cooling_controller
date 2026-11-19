#include "operation_manager.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "esp_timer.h"

#include "device_status.h"
#include "fan_control.h"
#include "metrics.h"
#include "operation_status_ble.h"
#include "operation_preflight.h"
#include "output_limit.h"
#include "params.h"
#include "state.h"

#define OP_CALIB_BASELINE_WAIT_US (4 * 1000000LL)
#define OP_CALIB_STOP_WAIT_US (6 * 1000000LL)
#define OP_CALIB_INCREMENT_PAUSE_US (3 * 1000000LL)
#define OP_CALIB_STEP_WAIT_US (4 * 1000000LL)
#define OP_CALIB_OBSERVATION_MAX_AGE_US (METRICS_TACH_SCAN_US + METRICS_TACH_CHANNEL_WINDOW_MS * 1000LL)
#define OP_CALIB_ROTATION_WINDOWS 3
#define OP_CALIB_ROTATION_TIMEOUT_US (OP_CALIB_ROTATION_WINDOWS * METRICS_TACH_SCAN_US + OP_CALIB_OBSERVATION_MAX_AGE_US)
#define OP_CALIB_STEP_DELTA 5
#define OP_CALIB_RPM_DELTA 50.0f
#define OP_OUTPUT_CONFIRM_TIMEOUT_US (9 * 1000000LL)
#define OP_CALIB_START_SPEED 10
#define OP_CALIB_MAX_SPEED_OFFSET 30.0f
#define OP_SETUP_WAIT_US (6 * 1000000LL)
#define OP_SETUP_PWM_TARGET_OFFSET 30.0f
#define OP_SETUP_RPM_DROP 50.0f

typedef enum {
    CALIB_PHASE_IDLE = 0,
    CALIB_PHASE_WAIT_STOP_OUTPUT,
    CALIB_PHASE_STOP_WAIT,
    CALIB_PHASE_WAIT_OUTPUT,
    CALIB_PHASE_WAIT_BASELINE,
    CALIB_PHASE_CONFIRM_ROTATION,
    CALIB_PHASE_ROTATION_CONFIRMED,
} calib_phase_t;

typedef enum {
    SETUP_PHASE_IDLE = 0,
    SETUP_PHASE_WAIT_DC,
    SETUP_PHASE_WAIT_PWM,
    SETUP_PHASE_CALIBRATION,
} setup_phase_t;

typedef enum {
    CALIB_EVENT_STARTED,
    CALIB_EVENT_STOP_APPLIED,
    CALIB_EVENT_STEP_REQUESTED,
    CALIB_EVENT_BASELINE_APPLIED,
    CALIB_EVENT_BASELINE_OBSERVED,
    CALIB_EVENT_STEP_APPLIED,
    CALIB_EVENT_OUTPUT_INTERRUPTED,
    CALIB_EVENT_ROTATION_CONFIRMED,
    CALIB_EVENT_ROTATION_LOST,
    CALIB_EVENT_ROTATION_TIMED_OUT,
    CALIB_EVENT_FINISHED,
} calib_event_t;

typedef enum {
    SETUP_EVENT_DC_OUTPUT_SET,
    SETUP_EVENT_PWM_OUTPUT_SET,
    SETUP_EVENT_CALIBRATION_STARTED,
    SETUP_EVENT_FINISHED,
} setup_event_t;

typedef enum {
    OP_STEP_CONTINUE = 0,
    OP_STEP_DONE = 1,
    OP_STEP_ERROR = 2,
} op_step_result_t;

typedef bool (*op_start_fn)(const char **err_text);
typedef op_step_result_t (*op_step_fn)(int64_t now_us, const char **err_text);
typedef void (*op_finish_fn)(void);

typedef struct {
    operation_type_t type;
    op_start_fn start;
    op_step_fn step;
    op_finish_fn finish;
} op_def_t;

static void operation_manager_publish_progress(const char *text);

static calib_phase_t g_calib_phase = CALIB_PHASE_IDLE;
static int64_t g_calib_next_check_us = 0;
static int64_t g_calib_observe_from_us;
static int64_t g_calib_output_deadline_us;
static float g_calib_baseline_rpm = 0.0f;
static uint8_t g_calib_target = 0;
static uint8_t g_calib_pending_target = 0;
static struct {
    int64_t completed_us;
    uint8_t consecutive;
} g_calib_rotation[METRICS_FAN_CHANNELS];
static setup_phase_t g_setup_phase = SETUP_PHASE_IDLE;
static int64_t g_setup_next_check_us = 0;
static float g_setup_dc_rpm[METRICS_FAN_CHANNELS] = {0};
static bool g_setup_active[METRICS_FAN_CHANNELS] = {false};
static float g_preflight_dc_max_percent = 100.0f;
static float g_preflight_pwm_max_percent = 100.0f;
static params_t g_operation_params;
static uint32_t g_operation_revision;
static int64_t g_output_confirm_deadline_us;

static void op_calib_dispatch(calib_event_t event) {
    switch (event) {
        case CALIB_EVENT_STARTED:
            if (g_calib_phase == CALIB_PHASE_IDLE) g_calib_phase = CALIB_PHASE_WAIT_STOP_OUTPUT;
            break;
        case CALIB_EVENT_STOP_APPLIED:
            if (g_calib_phase == CALIB_PHASE_WAIT_STOP_OUTPUT) g_calib_phase = CALIB_PHASE_STOP_WAIT;
            break;
        case CALIB_EVENT_STEP_REQUESTED:
            if (g_calib_phase == CALIB_PHASE_STOP_WAIT) g_calib_phase = CALIB_PHASE_WAIT_OUTPUT;
            break;
        case CALIB_EVENT_BASELINE_APPLIED:
            if (g_calib_phase == CALIB_PHASE_WAIT_OUTPUT) g_calib_phase = CALIB_PHASE_WAIT_BASELINE;
            break;
        case CALIB_EVENT_BASELINE_OBSERVED:
            if (g_calib_phase == CALIB_PHASE_WAIT_BASELINE) g_calib_phase = CALIB_PHASE_WAIT_STOP_OUTPUT;
            break;
        case CALIB_EVENT_STEP_APPLIED:
            if (g_calib_phase == CALIB_PHASE_WAIT_OUTPUT) g_calib_phase = CALIB_PHASE_CONFIRM_ROTATION;
            break;
        case CALIB_EVENT_OUTPUT_INTERRUPTED:
            if (g_calib_phase == CALIB_PHASE_STOP_WAIT) g_calib_phase = CALIB_PHASE_WAIT_STOP_OUTPUT;
            if (g_calib_phase == CALIB_PHASE_WAIT_BASELINE || g_calib_phase == CALIB_PHASE_CONFIRM_ROTATION ||
                g_calib_phase == CALIB_PHASE_ROTATION_CONFIRMED) {
                g_calib_phase = CALIB_PHASE_WAIT_OUTPUT;
            }
            break;
        case CALIB_EVENT_ROTATION_CONFIRMED:
            if (g_calib_phase == CALIB_PHASE_CONFIRM_ROTATION) g_calib_phase = CALIB_PHASE_ROTATION_CONFIRMED;
            break;
        case CALIB_EVENT_ROTATION_LOST:
            if (g_calib_phase == CALIB_PHASE_ROTATION_CONFIRMED) g_calib_phase = CALIB_PHASE_CONFIRM_ROTATION;
            break;
        case CALIB_EVENT_ROTATION_TIMED_OUT:
            if (g_calib_phase == CALIB_PHASE_CONFIRM_ROTATION) g_calib_phase = CALIB_PHASE_WAIT_STOP_OUTPUT;
            break;
        case CALIB_EVENT_FINISHED:
            g_calib_phase = CALIB_PHASE_IDLE;
            break;
    }
}

static void op_setup_dispatch(setup_event_t event) {
    switch (event) {
        case SETUP_EVENT_DC_OUTPUT_SET:
            if (g_setup_phase == SETUP_PHASE_IDLE) g_setup_phase = SETUP_PHASE_WAIT_DC;
            break;
        case SETUP_EVENT_PWM_OUTPUT_SET:
            if (g_setup_phase == SETUP_PHASE_WAIT_DC) g_setup_phase = SETUP_PHASE_WAIT_PWM;
            break;
        case SETUP_EVENT_CALIBRATION_STARTED:
            if (g_setup_phase == SETUP_PHASE_WAIT_PWM) g_setup_phase = SETUP_PHASE_CALIBRATION;
            break;
        case SETUP_EVENT_FINISHED:
            g_setup_phase = SETUP_PHASE_IDLE;
            break;
    }
}

static bool op_calib_observe_fan(uint8_t channel, int64_t now_us, metrics_fan_observation_t *observation) {
    return metrics_get_fan_observation(channel, observation) &&
           observation->started_us >= g_calib_observe_from_us &&
           now_us - observation->completed_us <= OP_CALIB_OBSERVATION_MAX_AGE_US;
}

static float op_clamp_percent(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 100.0f) return 100.0f;
    return value;
}

static float op_max_percent_for_control(uint8_t control_type) {
    return control_type == PARAM_FAN_CONTROL_PWM ? g_preflight_pwm_max_percent : g_preflight_dc_max_percent;
}

static float op_percent_from_control_max(uint8_t control_type, float offset_percent) {
    return op_clamp_percent(op_max_percent_for_control(control_type) - offset_percent);
}

static uint8_t op_calib_max_speed(void) {
    float max_speed = op_percent_from_control_max(g_operation_params.fan_control_type, OP_CALIB_MAX_SPEED_OFFSET);
    if (max_speed < (float)OP_CALIB_START_SPEED) {
        max_speed = (float)OP_CALIB_START_SPEED;
    }
    return (uint8_t)lroundf(max_speed);
}

static bool op_calib_fan_monitoring_enabled(const params_t *params, uint8_t channel) {
    if (!params) return false;
    switch (channel) {
        case 0:
            return true;
        case 1:
            return params->fan2_monitoring_enabled != 0;
        case 2:
            return params->fan3_monitoring_enabled != 0;
        case 3:
            return params->fan4_monitoring_enabled != 0;
        default:
            return false;
    }
}

static const char *op_calib_rotation_error(int64_t now_us) {
    static const char *const errors[METRICS_FAN_CHANNELS] = {
        "fan 1 RPM unstable", "fan 2 RPM unstable", "fan 3 RPM unstable", "fan 4 RPM unstable",
    };
    const char *error = NULL;
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        if (!op_calib_fan_monitoring_enabled(&g_operation_params, ch)) continue;

        metrics_fan_observation_t observation;
        if (!op_calib_observe_fan(ch, now_us, &observation)) {
            g_calib_rotation[ch].consecutive = 0;
        } else if (observation.completed_us > g_calib_rotation[ch].completed_us) {
            if (observation.completed_us - g_calib_rotation[ch].completed_us > OP_CALIB_OBSERVATION_MAX_AGE_US) {
                g_calib_rotation[ch].consecutive = 0;
            }
            g_calib_rotation[ch].completed_us = observation.completed_us;
            bool spinning = isfinite(observation.rpm) && observation.rpm > 0.0f;
            bool enough_rpm = ch != 0 || observation.rpm >= g_calib_baseline_rpm + OP_CALIB_RPM_DELTA;
            if (!spinning || !enough_rpm) {
                g_calib_rotation[ch].consecutive = 0;
            } else if (g_calib_rotation[ch].consecutive < OP_CALIB_ROTATION_WINDOWS) {
                g_calib_rotation[ch].consecutive++;
            }
        }
        if (!error && g_calib_rotation[ch].consecutive < OP_CALIB_ROTATION_WINDOWS) error = errors[ch];
    }
    return error;
}

static void op_calib_publish_step(float rpm) {
    char msg[OP_ERROR_TEXT_MAX + 1];
    uint16_t rpm_u = (uint16_t)fminf(fmaxf(rpm, 0.0f), 65535.0f);
    snprintf(msg, sizeof(msg), "calib S%03hu R%05hu", (uint16_t)g_calib_target, rpm_u);
    operation_manager_publish_progress(msg);
}

static bool op_calib_apply_target(uint8_t target, const char **err_text) {
    uint8_t calib_max_speed = op_calib_max_speed();
    if (target < OP_CALIB_START_SPEED) {
        target = OP_CALIB_START_SPEED;
    }
    if (target > calib_max_speed) {
        target = calib_max_speed;
    }
    if (!fan_control_override_set_output(operation_manager_get_active_type(),
                                         g_operation_params.fan_control_type, (float)target)) {
        if (err_text) *err_text = "fan busy";
        return false;
    }
    g_calib_target = target;
    g_calib_output_deadline_us = esp_timer_get_time() + OP_OUTPUT_CONFIRM_TIMEOUT_US;
    op_calib_dispatch(CALIB_EVENT_STEP_REQUESTED);
    return true;
}

static bool op_calib_request_stop(uint8_t next_target, calib_event_t event, const char **err_text) {
    if (!fan_control_override_set_output(operation_manager_get_active_type(),
                                         g_operation_params.fan_control_type, 0.0f)) {
        if (err_text) *err_text = "fan busy";
        return false;
    }
    g_calib_pending_target = next_target;
    g_calib_output_deadline_us = esp_timer_get_time() + OP_OUTPUT_CONFIRM_TIMEOUT_US;
    op_calib_dispatch(event);
    return true;
}

static bool op_calibration_begin(const params_t *params, const char **err_text) {
    g_operation_params = *params;
    g_calib_target = 0;
    g_calib_baseline_rpm = 0.0f;
    return op_calib_request_stop(0, CALIB_EVENT_STARTED, err_text);
}

static bool op_calibration_start(const char **err_text) {
    params_t current;
    if (!params_read(&current)) {
        if (err_text) *err_text = "params read";
        return false;
    }
    return op_calibration_begin(&current, err_text);
}

static op_step_result_t op_calibration_step(int64_t now_us, const char **err_text) {
    if (g_calib_phase == CALIB_PHASE_WAIT_STOP_OUTPUT || g_calib_phase == CALIB_PHASE_WAIT_OUTPUT) {
        return OP_STEP_CONTINUE;
    }
    if (g_calib_phase == CALIB_PHASE_STOP_WAIT) {
        if (now_us < g_calib_next_check_us) return OP_STEP_CONTINUE;
        uint8_t target = g_calib_pending_target ? g_calib_pending_target : OP_CALIB_START_SPEED;
        if (!op_calib_apply_target(target, err_text)) return OP_STEP_ERROR;
        g_calib_pending_target = 0;
        return OP_STEP_CONTINUE;
    }
    if (g_calib_phase == CALIB_PHASE_WAIT_BASELINE) {
        if (now_us < g_calib_observe_from_us) return OP_STEP_CONTINUE;
        metrics_fan_observation_t observation;
        if (!op_calib_observe_fan(0, now_us, &observation)) {
            if (now_us < g_calib_next_check_us) return OP_STEP_CONTINUE;
            if (err_text) *err_text = "fan 1 no tach data";
            return OP_STEP_ERROR;
        }
        g_calib_baseline_rpm = isfinite(observation.rpm) ? observation.rpm : 0.0f;
        op_calib_publish_step(g_calib_baseline_rpm);
        uint8_t next = g_calib_target + OP_CALIB_STEP_DELTA;
        if (next > op_calib_max_speed()) next = op_calib_max_speed();
        if (next <= g_calib_target) {
            if (err_text) *err_text = "no RPM test range";
            return OP_STEP_ERROR;
        }
        return op_calib_request_stop(next, CALIB_EVENT_BASELINE_OBSERVED, err_text) ?
               OP_STEP_CONTINUE : OP_STEP_ERROR;
    }
    if (g_calib_phase == CALIB_PHASE_CONFIRM_ROTATION) {
        if (now_us < g_calib_observe_from_us) return OP_STEP_CONTINUE;
        const char *rotation_error = op_calib_rotation_error(now_us);
        if (rotation_error && now_us < g_calib_next_check_us) return OP_STEP_CONTINUE;

        metrics_fan_observation_t observation;
        op_calib_publish_step(op_calib_observe_fan(0, now_us, &observation) ? observation.rpm : 0.0f);
        if (!rotation_error) {
            g_operation_params.fan_min_speed = (int32_t)g_calib_target;
            op_calib_dispatch(CALIB_EVENT_ROTATION_CONFIRMED);
            return OP_STEP_DONE;
        }
        uint8_t calib_max_speed = op_calib_max_speed();
        if (g_calib_target >= calib_max_speed) {
            if (err_text) *err_text = rotation_error;
            return OP_STEP_ERROR;
        }
        uint8_t next = g_calib_target + OP_CALIB_STEP_DELTA;
        if (next > calib_max_speed) next = calib_max_speed;
        return op_calib_request_stop(next, CALIB_EVENT_ROTATION_TIMED_OUT, err_text) ?
               OP_STEP_CONTINUE : OP_STEP_ERROR;
    }
    if (err_text) *err_text = "calib state";
    return OP_STEP_ERROR;
}

static const char *op_calib_check_output(float applied, uint8_t control_type, output_limit_status_t limit) {
    int64_t now_us = esp_timer_get_time();
    if (output_limit_is_adjusting(limit.state)) {
        op_calib_dispatch(CALIB_EVENT_OUTPUT_INTERRUPTED);
        g_calib_output_deadline_us = now_us + OP_OUTPUT_CONFIRM_TIMEOUT_US;
        return NULL;
    }
    if (limit.state != OUTPUT_LIMIT_NORMAL && limit.state != OUTPUT_LIMIT_LIMITED) return "output limit alarm";

    bool stopping = g_calib_phase == CALIB_PHASE_WAIT_STOP_OUTPUT || g_calib_phase == CALIB_PHASE_STOP_WAIT;
    uint8_t target = stopping ? 0 : g_calib_target;
    if (limit.effective < target) return "limit below target";
    // With power off the hardware retains the control type of the last nonzero output.
    if (applied != (float)target || (!stopping && control_type != g_operation_params.fan_control_type)) {
        if (g_calib_phase != CALIB_PHASE_WAIT_OUTPUT && g_calib_phase != CALIB_PHASE_WAIT_STOP_OUTPUT) {
            op_calib_dispatch(CALIB_EVENT_OUTPUT_INTERRUPTED);
            g_calib_output_deadline_us = now_us + OP_OUTPUT_CONFIRM_TIMEOUT_US;
        }
        return now_us >= g_calib_output_deadline_us ? "output check timeout" : NULL;
    }
    if (g_calib_phase == CALIB_PHASE_WAIT_STOP_OUTPUT) {
        g_calib_next_check_us = now_us + (g_calib_pending_target ? OP_CALIB_INCREMENT_PAUSE_US : OP_CALIB_STOP_WAIT_US);
        op_calib_dispatch(CALIB_EVENT_STOP_APPLIED);
    } else if (g_calib_phase == CALIB_PHASE_WAIT_OUTPUT) {
        if (target == OP_CALIB_START_SPEED) {
            g_calib_observe_from_us = now_us + OP_CALIB_BASELINE_WAIT_US;
            g_calib_next_check_us = g_calib_observe_from_us + OP_CALIB_OBSERVATION_MAX_AGE_US +
                                    METRICS_TACH_CHANNEL_WINDOW_MS * 1000LL;
            op_calib_dispatch(CALIB_EVENT_BASELINE_APPLIED);
        } else {
            memset(g_calib_rotation, 0, sizeof(g_calib_rotation));
            g_calib_observe_from_us = now_us + OP_CALIB_STEP_WAIT_US;
            g_calib_next_check_us = g_calib_observe_from_us + OP_CALIB_ROTATION_TIMEOUT_US;
            op_calib_dispatch(CALIB_EVENT_STEP_APPLIED);
        }
    }
    return NULL;
}

static void op_calibration_finish(void) {
    op_calib_dispatch(CALIB_EVENT_FINISHED);
    g_calib_next_check_us = 0;
    g_calib_baseline_rpm = 0.0f;
    g_calib_target = 0;
    g_calib_pending_target = 0;
}

static void op_setup_reset(void) {
    op_setup_dispatch(SETUP_EVENT_FINISHED);
    g_setup_next_check_us = 0;
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        g_setup_dc_rpm[ch] = 0.0f;
        g_setup_active[ch] = false;
    }
}

static bool op_setup_start(const char **err_text) {
    op_setup_reset();
    if (!fan_control_override_set_output(OP_TYPE_SETUP_FANS,
                                         PARAM_FAN_CONTROL_DC,
                                         op_max_percent_for_control(PARAM_FAN_CONTROL_DC))) {
        if (err_text) *err_text = "fan busy";
        return false;
    }
    g_setup_next_check_us = esp_timer_get_time() + OP_SETUP_WAIT_US;
    op_setup_dispatch(SETUP_EVENT_DC_OUTPUT_SET);
    return true;
}

static op_step_result_t op_setup_step(int64_t now_us, const char **err_text) {
    if (g_setup_phase == SETUP_PHASE_WAIT_DC) {
        if (now_us < g_setup_next_check_us) {
            return OP_STEP_CONTINUE;
        }

        bool any_active = false;
        for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
            float rpm = metrics_get_fan_speed_rpm_channel(ch);
            g_setup_dc_rpm[ch] = isfinite(rpm) ? rpm : 0.0f;
            g_setup_active[ch] = isfinite(rpm) && rpm > 0.0f;
            any_active = any_active || g_setup_active[ch];
        }
        if (!any_active) {
            if (err_text) *err_text = "no fans";
            return OP_STEP_ERROR;
        }

        if (!fan_control_override_set_output(OP_TYPE_SETUP_FANS,
                                             PARAM_FAN_CONTROL_PWM,
                                             op_percent_from_control_max(PARAM_FAN_CONTROL_PWM,
                                                                         OP_SETUP_PWM_TARGET_OFFSET))) {
            if (err_text) *err_text = "fan busy";
            return OP_STEP_ERROR;
        }
        g_setup_next_check_us = esp_timer_get_time() + OP_SETUP_WAIT_US;
        op_setup_dispatch(SETUP_EVENT_PWM_OUTPUT_SET);
        return OP_STEP_CONTINUE;
    }

    if (g_setup_phase == SETUP_PHASE_WAIT_PWM) {
        if (now_us < g_setup_next_check_us) {
            return OP_STEP_CONTINUE;
        }

        bool all_active_dropped = true;
        for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
            if (!g_setup_active[ch]) {
                continue;
            }
            float rpm = metrics_get_fan_speed_rpm_channel(ch);
            float pwm_rpm = isfinite(rpm) ? rpm : 0.0f;
            if (pwm_rpm > g_setup_dc_rpm[ch] - OP_SETUP_RPM_DROP) {
                all_active_dropped = false;
                break;
            }
        }

        params_t current;
        if (!params_read(&current)) {
            if (err_text) *err_text = "params read";
            return OP_STEP_ERROR;
        }
        current.fan_control_type = all_active_dropped ? PARAM_FAN_CONTROL_PWM : PARAM_FAN_CONTROL_DC;
        current.fan_monitoring_enabled = g_setup_active[0];
        current.fan2_monitoring_enabled = g_setup_active[1];
        current.fan3_monitoring_enabled = g_setup_active[2];
        current.fan4_monitoring_enabled = g_setup_active[3];

        if (!op_calibration_begin(&current, err_text)) {
            return OP_STEP_ERROR;
        }
        op_setup_dispatch(SETUP_EVENT_CALIBRATION_STARTED);
        return OP_STEP_CONTINUE;
    }

    if (g_setup_phase == SETUP_PHASE_CALIBRATION) {
        return op_calibration_step(now_us, err_text);
    }

    if (err_text) *err_text = "setup state";
    return OP_STEP_ERROR;
}

static void op_setup_finish(void) {
    op_setup_reset();
    op_calibration_finish();
}

static const op_def_t g_op_defs[] = {
    {
        .type = OP_TYPE_SETUP_FANS,
        .start = op_setup_start,
        .step = op_setup_step,
        .finish = op_setup_finish,
    },
    {
        .type = OP_TYPE_FAN_CALIBRATION,
        .start = op_calibration_start,
        .step = op_calibration_step,
        .finish = op_calibration_finish,
    },
};

typedef struct {
    operation_type_t type;
    operation_state_t state;
    uint8_t text_len;
    char text[OP_ERROR_TEXT_MAX];
} operation_status_t;

typedef enum {
    OP_PHASE_READY = 0,
    OP_PHASE_QUEUED,
    OP_PHASE_PREFLIGHT,
    OP_PHASE_RUNNING,
    OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION,
    OP_PHASE_WAITING_FOR_OUTPUT_LIMIT,
    OP_PHASE_CANCELLING,
    OP_PHASE_COMMITTING,
} operation_phase_t;

typedef enum {
    OP_EVENT_START,
    OP_EVENT_BEGIN_PREFLIGHT,
    OP_EVENT_PREFLIGHT_PASSED,
    OP_EVENT_CANCEL,
    OP_EVENT_OWNER_DISCONNECTED,
    OP_EVENT_STEPS_SUCCEEDED,
    OP_EVENT_OUTPUT_LIMIT_ADJUSTING,
    OP_EVENT_OUTPUT_LIMIT_RESOLVED,
    OP_EVENT_ROTATION_LOST,
    OP_EVENT_OUTPUT_CONFIRMED,
    OP_EVENT_FINISHED,
} operation_event_t;

typedef struct {
    operation_phase_t phase;
    operation_type_t type;
    uint16_t owner;
} operation_context_t;

static operation_status_t g_status;
static operation_context_t g_operation;

// All phase transitions run under state_lock. The fan task owns operation effects.
static bool operation_dispatch_locked(operation_event_t event, uint16_t owner) {
    operation_phase_t phase = g_operation.phase;
    switch (event) {
        case OP_EVENT_START:
            if (phase != OP_PHASE_READY) return false;
            g_operation.phase = OP_PHASE_QUEUED;
            return true;
        case OP_EVENT_BEGIN_PREFLIGHT:
            if (phase != OP_PHASE_QUEUED) return false;
            g_operation.phase = OP_PHASE_PREFLIGHT;
            return true;
        case OP_EVENT_PREFLIGHT_PASSED:
            if (phase != OP_PHASE_PREFLIGHT) return false;
            g_operation.phase = OP_PHASE_RUNNING;
            return true;
        case OP_EVENT_OWNER_DISCONNECTED:
        case OP_EVENT_CANCEL:
            if (phase == OP_PHASE_READY || owner != g_operation.owner ||
                owner == BLE_HS_CONN_HANDLE_NONE) return true;
            if (event == OP_EVENT_OWNER_DISCONNECTED) {
                g_operation.owner = BLE_HS_CONN_HANDLE_NONE;
            }
            if (phase == OP_PHASE_COMMITTING) return false;
            g_operation.phase = OP_PHASE_CANCELLING;
            return true;
        case OP_EVENT_STEPS_SUCCEEDED:
            if (phase != OP_PHASE_RUNNING) return false;
            g_operation.phase = OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION;
            return true;
        case OP_EVENT_OUTPUT_LIMIT_ADJUSTING:
            if (phase != OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION) return false;
            g_operation.phase = OP_PHASE_WAITING_FOR_OUTPUT_LIMIT;
            return true;
        case OP_EVENT_OUTPUT_LIMIT_RESOLVED:
            if (phase != OP_PHASE_WAITING_FOR_OUTPUT_LIMIT) return false;
            g_operation.phase = OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION;
            return true;
        case OP_EVENT_ROTATION_LOST:
            if (phase != OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION) return false;
            g_operation.phase = OP_PHASE_RUNNING;
            return true;
        case OP_EVENT_OUTPUT_CONFIRMED:
            if (phase != OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION) return false;
            g_operation.phase = OP_PHASE_COMMITTING;
            return true;
        case OP_EVENT_FINISHED:
            if (phase == OP_PHASE_READY || phase == OP_PHASE_QUEUED) return false;
            g_operation = (operation_context_t){.phase = OP_PHASE_READY};
            return true;
    }
    return false;
}

static const op_def_t *operation_manager_get_def(operation_type_t type) {
    for (size_t i = 0; i < sizeof(g_op_defs) / sizeof(g_op_defs[0]); i++) {
        if (g_op_defs[i].type == type) {
            return &g_op_defs[i];
        }
    }
    return NULL;
}

static operation_status_t operation_manager_status(void) {
    state_lock();
    operation_status_t status = g_status;
    state_unlock();
    return status;
}

static void operation_manager_encode(const operation_status_t *status, uint8_t *out) {
    out[0] = OP_STATUS_VERSION;
    out[1] = (uint8_t)status->type;
    out[2] = (uint8_t)status->state;
    out[3] = status->text_len;
    memset(out + 4, 0, OP_ERROR_TEXT_MAX);
    memcpy(out + 4, status->text, status->text_len);
}

static void operation_manager_notify(const operation_status_t *status, uint16_t owner) {
    uint8_t payload[OP_STATUS_PAYLOAD_LEN];
    operation_manager_encode(status, payload);
    operation_status_notify(owner, payload, sizeof(payload));
}

static operation_status_t operation_manager_report_locked(operation_state_t state, const char *text) {
    g_status.type = g_operation.type;
    g_status.state = state;
    g_status.text_len = text ? (uint8_t)strnlen(text, OP_ERROR_TEXT_MAX) : 0;
    if (g_status.text_len > 0) {
        memcpy(g_status.text, text, g_status.text_len);
    }
    return g_status;
}

static void operation_manager_publish_progress(const char *text) {
    state_lock();
    bool running = g_operation.phase == OP_PHASE_RUNNING;
    uint16_t owner = g_operation.owner;
    operation_status_t status = g_status;
    if (running) {
        status = operation_manager_report_locked(OP_STATE_IN_SERVICE, text);
    }
    state_unlock();
    if (running) operation_manager_notify(&status, owner);
}

void operation_manager_init(void) {
    g_status = (operation_status_t){.type = OP_TYPE_NONE, .state = OP_STATE_IDLE};
    g_operation = (operation_context_t){.phase = OP_PHASE_READY};
    operation_preflight_reset();
}

operation_state_t operation_manager_get_state(void) {
    return operation_manager_status().state;
}

operation_type_t operation_manager_get_active_type(void) {
    operation_status_t status = operation_manager_status();
    return status.state == OP_STATE_IN_SERVICE ? status.type : OP_TYPE_NONE;
}

bool operation_manager_is_busy(void) {
    state_lock();
    bool busy = g_operation.phase != OP_PHASE_READY;
    state_unlock();
    return busy;
}

void operation_manager_get_status_payload(uint8_t *out, size_t len) {
    if (!out || len < OP_STATUS_PAYLOAD_LEN) return;
    operation_status_t status = operation_manager_status();
    operation_manager_encode(&status, out);
}

operation_start_result_t operation_manager_start(operation_type_t type, uint16_t owner) {
    if (!operation_manager_get_def(type) || owner == BLE_HS_CONN_HANDLE_NONE) {
        return OP_START_INVALID;
    }
    if (device_status_has_error_flag(DEVICE_ERROR_OVERHEAT)) return OP_START_OVERHEAT;
    state_lock();
    if (!operation_dispatch_locked(OP_EVENT_START, owner)) {
        state_unlock();
        return OP_START_BUSY;
    }
    g_operation.type = type;
    g_operation.owner = owner;
    g_operation_revision = params_reset_revision();
    state_unlock();
    return OP_START_OK;
}

bool operation_manager_cancel(uint16_t owner) {
    state_lock();
    bool accepted = operation_dispatch_locked(OP_EVENT_CANCEL, owner);
    state_unlock();
    return accepted;
}

void operation_manager_on_disconnect(uint16_t owner) {
    state_lock();
    operation_dispatch_locked(OP_EVENT_OWNER_DISCONNECTED, owner);
    state_unlock();
}

static bool operation_manager_advance(operation_event_t event) {
    state_lock();
    bool accepted = operation_dispatch_locked(event, BLE_HS_CONN_HANDLE_NONE);
    state_unlock();
    return accepted;
}

static const char *operation_manager_commit(operation_type_t type) {
    if (!operation_manager_advance(OP_EVENT_OUTPUT_CONFIRMED)) return "cancelled";

    uint16_t mask = PARAM_MASK_FAN_MIN_SPEED;
    if (type == OP_TYPE_SETUP_FANS) {
        mask |= PARAM_MASK_FAN_CONTROL_TYPE | PARAM_MASK_FAN_MONITORING_ENABLED |
                PARAM_MASK_FAN2_MONITORING_ENABLED | PARAM_MASK_FAN3_MONITORING_ENABLED |
                PARAM_MASK_FAN4_MONITORING_ENABLED;
    }
    uint8_t status = params_apply_operation_result(&g_operation_params, mask, g_operation_revision);
    if (status == PARAM_STATUS_OK) return NULL;
    return status == PARAM_STATUS_BUSY ? "factory reset" : "params apply";
}

static void operation_manager_finish(const op_def_t *def, operation_state_t state,
                                     const char *text) {
    operation_preflight_reset();
    def->finish();
    g_operation_params = (params_t){0};
    fan_control_override_clear(def->type);

    state_lock();
    uint16_t owner = g_operation.owner;
    bool cancelled = g_operation.phase == OP_PHASE_CANCELLING;
    if (cancelled) {
        state = OP_STATE_ERROR;
        text = "cancelled";
    }
    operation_status_t status = operation_manager_report_locked(state, text);
    operation_dispatch_locked(OP_EVENT_FINISHED, owner);
    state_unlock();
    if (cancelled) output_limit_request_stop();
    operation_manager_notify(&status, owner);
}

void operation_manager_tick(int64_t now_us) {
    const char *err_text = NULL;
    state_lock();
    bool starting = operation_dispatch_locked(OP_EVENT_BEGIN_PREFLIGHT, BLE_HS_CONN_HANDLE_NONE);
    operation_context_t current = g_operation;
    operation_status_t status = g_status;
    if (starting) {
        status = operation_manager_report_locked(OP_STATE_IN_SERVICE, NULL);
    }
    state_unlock();

    if (current.phase == OP_PHASE_READY) return;
    const op_def_t *def = operation_manager_get_def(current.type);
    if (current.phase == OP_PHASE_CANCELLING) {
        operation_manager_finish(def, OP_STATE_ERROR, "cancelled");
        return;
    }
    if (starting) operation_manager_notify(&status, current.owner);
    operation_type_t type = current.type;
    if (device_status_has_error_flag(DEVICE_ERROR_OVERHEAT)) {
        operation_manager_finish(def, OP_STATE_ERROR, "overheat");
        return;
    }
    if (device_status_has_error_flag(DEVICE_ERROR_FAN_OUTPUT)) {
        operation_manager_finish(def, OP_STATE_ERROR, "fan output fault");
        return;
    }
    if (g_operation_revision != params_reset_revision()) {
        operation_manager_finish(def, OP_STATE_ERROR, "factory reset");
        return;
    }
    if (current.phase == OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION ||
        current.phase == OP_PHASE_WAITING_FOR_OUTPUT_LIMIT) return;
    if (starting) {
        g_preflight_dc_max_percent = 100.0f;
        g_preflight_pwm_max_percent = 100.0f;
        if (!operation_preflight_start(type, now_us, &err_text)) {
            operation_manager_finish(def, OP_STATE_ERROR, err_text ? err_text : "preflight start");
            return;
        }
    }
    if (operation_preflight_is_active()) {
        operation_preflight_result_t result = operation_preflight_step(now_us, &err_text);
        if (result == OP_PREFLIGHT_RESULT_CONTINUE) {
            return;
        }
        if (result == OP_PREFLIGHT_RESULT_ERROR) {
            operation_manager_finish(def, OP_STATE_ERROR, err_text ? err_text : "preflight failed");
            return;
        }
        operation_preflight_caps_t caps;
        if (operation_preflight_get_caps(&caps)) {
            g_preflight_dc_max_percent = caps.dc_max_percent;
            g_preflight_pwm_max_percent = caps.pwm_max_percent;
        }
    }

    if (current.phase == OP_PHASE_PREFLIGHT) {
        if (!operation_manager_advance(OP_EVENT_PREFLIGHT_PASSED)) {
            operation_manager_finish(def, OP_STATE_ERROR, "cancelled");
            return;
        }
        if (!def->start(&err_text)) {
            operation_manager_finish(def, OP_STATE_ERROR, err_text ? err_text : "start failed");
            return;
        }
    }

    output_limit_state_t limit = output_limit_get_status().state;
    if (limit == OUTPUT_LIMIT_FAULT ||
        limit == OUTPUT_LIMIT_VERIFY_TIMEOUT || limit == OUTPUT_LIMIT_MINIMUM ||
        limit == OUTPUT_LIMIT_START_BLOCKED) {
        operation_manager_finish(def, OP_STATE_ERROR, "output limit alarm");
        return;
    }
    if (limit != OUTPUT_LIMIT_NORMAL && limit != OUTPUT_LIMIT_LIMITED) return;

    op_step_result_t result = def->step(now_us, &err_text);
    if (result == OP_STEP_DONE) {
        if (operation_manager_advance(OP_EVENT_STEPS_SUCCEEDED)) {
            g_output_confirm_deadline_us = now_us + OP_OUTPUT_CONFIRM_TIMEOUT_US;
        } else {
            operation_manager_finish(def, OP_STATE_ERROR, "cancelled");
        }
    } else if (result == OP_STEP_ERROR) {
        operation_manager_finish(def, OP_STATE_ERROR, err_text ? err_text : "op failed");
    }
}

void operation_manager_check_output(int64_t now_us, float applied, uint8_t control_type) {
    state_lock();
    operation_context_t current = g_operation;
    state_unlock();

    if (current.phase == OP_PHASE_RUNNING && g_calib_phase != CALIB_PHASE_IDLE) {
        const char *error = op_calib_check_output(applied, control_type, output_limit_get_status());
        if (error) operation_manager_finish(operation_manager_get_def(current.type), OP_STATE_ERROR, error);
        return;
    }
    if (current.phase != OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION &&
        current.phase != OP_PHASE_WAITING_FOR_OUTPUT_LIMIT) return;

    const op_def_t *def = operation_manager_get_def(current.type);
    output_limit_status_t limit = output_limit_get_status();
    const char *error = NULL;
    if (device_status_has_error_flag(DEVICE_ERROR_FAN_OUTPUT)) {
        error = "fan output fault";
    } else if (output_limit_is_adjusting(limit.state)) {
        if (g_calib_phase == CALIB_PHASE_ROTATION_CONFIRMED && current.phase == OP_PHASE_WAITING_FOR_OUTPUT_CONFIRMATION) {
            if (operation_manager_advance(OP_EVENT_ROTATION_LOST)) {
                op_calib_check_output(applied, control_type, limit);
            }
        } else {
            operation_manager_advance(OP_EVENT_OUTPUT_LIMIT_ADJUSTING);
        }
        return;
    } else if (limit.state != OUTPUT_LIMIT_NORMAL && limit.state != OUTPUT_LIMIT_LIMITED) {
        error = "output limit alarm";
    } else if (limit.effective < g_operation_params.fan_min_speed) {
        error = "limit below target";
    }

    if (error) {
        operation_manager_finish(def, OP_STATE_ERROR, error);
        return;
    }
    if (current.phase == OP_PHASE_WAITING_FOR_OUTPUT_LIMIT) {
        if (!operation_manager_advance(OP_EVENT_OUTPUT_LIMIT_RESOLVED)) return;
        g_output_confirm_deadline_us = now_us + OP_OUTPUT_CONFIRM_TIMEOUT_US;
    }

    if (control_type != g_operation_params.fan_control_type ||
        applied != (float)g_operation_params.fan_min_speed) {
        error = "output mismatch";
    }
    if (error) {
        operation_manager_finish(def, OP_STATE_ERROR, error);
        return;
    }
    if (g_calib_phase == CALIB_PHASE_ROTATION_CONFIRMED && op_calib_rotation_error(esp_timer_get_time())) {
        if (operation_manager_advance(OP_EVENT_ROTATION_LOST)) op_calib_dispatch(CALIB_EVENT_ROTATION_LOST);
        return;
    }
    if (limit.verified_output) {
        error = operation_manager_commit(current.type);
        operation_manager_finish(def, error ? OP_STATE_ERROR : OP_STATE_DONE, error);
        return;
    } else if (now_us >= g_output_confirm_deadline_us) {
        error = "output check timeout";
    }

    if (error) operation_manager_finish(def, OP_STATE_ERROR, error);
}
