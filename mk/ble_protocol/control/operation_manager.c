#include "operation_manager.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_timer.h"

#include "fan_control.h"
#include "metrics.h"
#include "operation_status_ble.h"
#include "params.h"
#include "state.h"

#define OP_CALIB_BASELINE_WAIT_US (4 * 1000000LL)
#define OP_CALIB_STOP_WAIT_US (6 * 1000000LL)
#define OP_CALIB_STEP_WAIT_US (2 * 1000000LL)
#define OP_CALIB_STEP_DELTA 5
#define OP_CALIB_RPM_DELTA 50.0f
#define OP_CALIB_START_SPEED 10
#define OP_CALIB_MAX_SPEED 70

typedef enum {
    CALIB_PHASE_IDLE = 0,
    CALIB_PHASE_STOP_WAIT,
    CALIB_PHASE_WAIT_BASELINE,
    CALIB_PHASE_RAMP,
} calib_phase_t;

typedef struct {
    bool uses_override;
    float override_rpm;
} op_guard_t;

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
    op_guard_t guard;
    op_start_fn start;
    op_step_fn step;
    op_finish_fn finish;
} op_def_t;

static void operation_manager_notify_custom(operation_type_t type,
                                            operation_state_t state,
                                            const char *err_text);

static int64_t g_calibration_sleep_until_us = 0;
static calib_phase_t g_calib_phase = CALIB_PHASE_IDLE;
static int64_t g_calib_next_check_us = 0;
static float g_calib_baseline_rpm = 0.0f;
static uint8_t g_calib_target = 0;

static float op_calib_read_rpm(void) {
    float rpm = metrics_get_fan_speed_rpm();
    if (!isfinite(rpm)) return 0.0f;
    return rpm;
}

static bool op_calib_apply_target(uint8_t target, const char **err_text) {
    if (target < OP_CALIB_START_SPEED) {
        target = OP_CALIB_START_SPEED;
    }
    if (target > OP_CALIB_MAX_SPEED) {
        target = OP_CALIB_MAX_SPEED;
    }
    if (!fan_control_override_set(OP_TYPE_FAN_CALIBRATION, (float)target)) {
        if (err_text) *err_text = "fan busy";
        return false;
    }
    g_calib_target = target;
    g_calib_next_check_us = esp_timer_get_time() + OP_CALIB_STEP_WAIT_US;
    return true;
}

static bool op_calibration_start(const char **err_text) {
    if (!fan_control_override_set(OP_TYPE_FAN_CALIBRATION, 0.0f)) {
        if (err_text) *err_text = "fan busy";
        return false;
    }
    g_calib_target = 0;
    g_calib_phase = CALIB_PHASE_STOP_WAIT;
    g_calib_baseline_rpm = 0.0f;
    g_calib_next_check_us = esp_timer_get_time() + OP_CALIB_STOP_WAIT_US;
    return true;
}

static op_step_result_t op_calibration_step(int64_t now_us, const char **err_text) {
    if (g_calib_phase == CALIB_PHASE_STOP_WAIT) {
        if (now_us < g_calib_next_check_us) {
            return OP_STEP_CONTINUE;
        }
        if (!op_calib_apply_target(OP_CALIB_START_SPEED, err_text)) {
            return OP_STEP_ERROR;
        }
        g_calib_phase = CALIB_PHASE_WAIT_BASELINE;
        g_calib_baseline_rpm = 0.0f;
        g_calib_next_check_us = esp_timer_get_time() + OP_CALIB_BASELINE_WAIT_US;
        return OP_STEP_CONTINUE;
    }
    if (g_calib_phase == CALIB_PHASE_WAIT_BASELINE) {
        if (now_us < g_calib_next_check_us) {
            return OP_STEP_CONTINUE;
        }
        g_calib_baseline_rpm = op_calib_read_rpm();
        g_calib_phase = CALIB_PHASE_RAMP;
        {
            char msg[OP_ERROR_TEXT_MAX + 1];
            uint16_t rpm_u = (uint16_t)fminf(fmaxf(g_calib_baseline_rpm, 0.0f), 65535.0f);
            uint16_t step_u = (uint16_t)g_calib_target;
            snprintf(msg, sizeof(msg), "calib S%03hu R%05hu", step_u, rpm_u);
            operation_manager_notify_custom(OP_TYPE_FAN_CALIBRATION, OP_STATE_IN_SERVICE, msg);
        }
        if (!op_calib_apply_target((uint8_t)(g_calib_target + OP_CALIB_STEP_DELTA), err_text)) {
            return OP_STEP_ERROR;
        }
        return OP_STEP_CONTINUE;
    }
    if (g_calib_phase == CALIB_PHASE_RAMP) {
        if (now_us < g_calib_next_check_us) {
            return OP_STEP_CONTINUE;
        }
        float rpm = op_calib_read_rpm();
        {
            char msg[OP_ERROR_TEXT_MAX + 1];
            uint16_t rpm_u = (uint16_t)fminf(fmaxf(rpm, 0.0f), 65535.0f);
            uint16_t step_u = (uint16_t)g_calib_target;
            snprintf(msg, sizeof(msg), "calib S%03hu R%05hu", step_u, rpm_u);
            operation_manager_notify_custom(OP_TYPE_FAN_CALIBRATION, OP_STATE_IN_SERVICE, msg);
        }
        if (rpm >= g_calib_baseline_rpm + OP_CALIB_RPM_DELTA) {
            params_t current;
            if (!params_cache_get(&current) && !params_read(&current)) {
                if (err_text) *err_text = "params read";
                return OP_STEP_ERROR;
            }
            current.fan_min_speed = (int32_t)g_calib_target;
            if (!params_write(&current, PARAM_MASK_FAN_MIN_SPEED)) {
                if (err_text) *err_text = "params write";
                return OP_STEP_ERROR;
            }
            if (params_apply(NULL) != PARAM_STATUS_OK) {
                if (err_text) *err_text = "params apply";
                return OP_STEP_ERROR;
            }
            return OP_STEP_DONE;
        }
        if (g_calib_target >= OP_CALIB_MAX_SPEED) {
            if (err_text) *err_text = "no rpm change";
            return OP_STEP_ERROR;
        }
        uint8_t next = g_calib_target + OP_CALIB_STEP_DELTA;
        if (next > OP_CALIB_MAX_SPEED) {
            next = OP_CALIB_MAX_SPEED;
        }
        if (!op_calib_apply_target(next, err_text)) {
            return OP_STEP_ERROR;
        }
        return OP_STEP_CONTINUE;
    }
    if (err_text) *err_text = "calib state";
    return OP_STEP_ERROR;
}

static void op_calibration_finish(void) {
    g_calibration_sleep_until_us = 0;
    g_calib_phase = CALIB_PHASE_IDLE;
    g_calib_next_check_us = 0;
    g_calib_baseline_rpm = 0.0f;
    g_calib_target = 0;
}

static const op_def_t g_op_defs[] = {
    {
        .type = OP_TYPE_FAN_CALIBRATION,
        .guard = {
            .uses_override = true,
            .override_rpm = 0.0f,
        },
        .start = op_calibration_start,
        .step = op_calibration_step,
        .finish = op_calibration_finish,
    },
};

static operation_state_t g_state = OP_STATE_IDLE;
static operation_type_t g_active = OP_TYPE_NONE;
static char g_error_text[OP_ERROR_TEXT_MAX + 1];
static uint8_t g_error_len = 0;
static QueueHandle_t g_op_cmd_q = NULL;
static uint32_t g_status_seq = 0;
typedef struct {
    operation_state_t state;
    operation_type_t type;
    uint8_t err_len;
    char err_text[OP_ERROR_TEXT_MAX];
} op_status_cache_t;
static op_status_cache_t g_status_cache = {0};

typedef struct {
    operation_type_t type;
} op_cmd_t;

static void operation_manager_cache_write(operation_type_t type,
                                          operation_state_t state,
                                          const char *err_text) {
    op_status_cache_t snap = {
        .state = state,
        .type = type,
        .err_len = 0,
        .err_text = {0},
    };
    if (err_text && err_text[0] != '\0') {
        size_t len = strnlen(err_text, OP_ERROR_TEXT_MAX);
        memcpy(snap.err_text, err_text, len);
        snap.err_len = (uint8_t)len;
    }
    __atomic_fetch_add(&g_status_seq, 1U, __ATOMIC_ACQ_REL);
    g_status_cache = snap;
    __atomic_fetch_add(&g_status_seq, 1U, __ATOMIC_ACQ_REL);
}

static void operation_manager_cache_read(op_status_cache_t *out) {
    if (!out) return;
    while (1) {
        uint32_t seq1 = __atomic_load_n(&g_status_seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1U) {
            continue;
        }
        op_status_cache_t snap = g_status_cache;
        uint32_t seq2 = __atomic_load_n(&g_status_seq, __ATOMIC_ACQUIRE);
        if (seq1 == seq2) {
            *out = snap;
            return;
        }
    }
}

static const op_def_t *operation_manager_get_def(operation_type_t type) {
    for (size_t i = 0; i < sizeof(g_op_defs) / sizeof(g_op_defs[0]); i++) {
        if (g_op_defs[i].type == type) {
            return &g_op_defs[i];
        }
    }
    return NULL;
}

static void operation_manager_set_error_locked(const char *text) {
    if (!text) {
        g_error_len = 0;
        g_error_text[0] = '\0';
        return;
    }
    size_t len = strnlen(text, OP_ERROR_TEXT_MAX);
    memcpy(g_error_text, text, len);
    g_error_text[len] = '\0';
    g_error_len = (uint8_t)len;
}

static void operation_manager_clear_error_locked(void) {
    g_error_len = 0;
    g_error_text[0] = '\0';
}

static void operation_manager_notify_custom(operation_type_t type,
                                            operation_state_t state,
                                            const char *err_text) {
    uint8_t payload[OP_STATUS_PAYLOAD_LEN];
    uint8_t err_len = 0;

    if (err_text && err_text[0] != '\0') {
        err_len = (uint8_t)strnlen(err_text, OP_ERROR_TEXT_MAX);
    }

    payload[0] = OP_STATUS_VERSION;
    payload[1] = (uint8_t)type;
    payload[2] = (uint8_t)state;
    payload[3] = err_len;
    memset(payload + 4, 0, OP_ERROR_TEXT_MAX);
    if (err_len > 0) {
        memcpy(payload + 4, err_text, err_len);
    }

    operation_manager_cache_write(type, state, err_text);
    uint16_t conn = fsm_get_conn_handle();
    operation_status_notify(conn, payload, sizeof(payload));
}

static void operation_manager_notify_state(operation_state_t state_override) {
    operation_type_t type;
    char err_buf[OP_ERROR_TEXT_MAX + 1];

    type = g_active;
    if (state_override == OP_STATE_ERROR && g_error_len > 0) {
        memcpy(err_buf, g_error_text, g_error_len);
        err_buf[g_error_len] = '\0';
    } else {
        err_buf[0] = '\0';
    }
    operation_manager_notify_custom(type, state_override, err_buf[0] ? err_buf : NULL);
}

static bool operation_manager_needs_override(operation_type_t type) {
    const op_def_t *def = operation_manager_get_def(type);
    return def != NULL && def->guard.uses_override;
}

void operation_manager_init(void) {
    g_state = OP_STATE_IDLE;
    g_active = OP_TYPE_NONE;
    operation_manager_clear_error_locked();
    if (!g_op_cmd_q) {
        g_op_cmd_q = xQueueCreate(4, sizeof(op_cmd_t));
    }
    operation_manager_cache_write(OP_TYPE_NONE, OP_STATE_IDLE, NULL);
}

operation_state_t operation_manager_get_state(void) {
    op_status_cache_t snap;
    operation_manager_cache_read(&snap);
    return snap.state;
}

operation_type_t operation_manager_get_active_type(void) {
    op_status_cache_t snap;
    operation_manager_cache_read(&snap);
    return snap.type;
}

bool operation_manager_is_active(void) {
    op_status_cache_t snap;
    operation_manager_cache_read(&snap);
    return snap.state == OP_STATE_IN_SERVICE;
}

void operation_manager_get_status_payload(uint8_t *out, size_t len) {
    if (!out || len < OP_STATUS_PAYLOAD_LEN) return;
    op_status_cache_t snap;
    operation_manager_cache_read(&snap);

    out[0] = OP_STATUS_VERSION;
    out[1] = (uint8_t)snap.type;
    out[2] = (uint8_t)snap.state;
    out[3] = snap.err_len;
    memset(out + 4, 0, OP_ERROR_TEXT_MAX);
    if (snap.err_len > 0) {
        memcpy(out + 4, snap.err_text, snap.err_len);
    }
}

static bool operation_manager_start_impl(operation_type_t type, const char **err_text) {
    const op_def_t *def = operation_manager_get_def(type);
    if (!def) return false;

    if (def->guard.uses_override) {
        if (!fan_control_override_set((uint8_t)type, def->guard.override_rpm)) {
            if (err_text) *err_text = "fan busy";
            return false;
        }
    }

    if (def->start) {
        const char *local_err = NULL;
        if (!def->start(&local_err)) {
            if (err_text) *err_text = local_err;
            return false;
        }
    }

    return true;
}

operation_start_result_t operation_manager_start(operation_type_t type) {
    if (!operation_manager_get_def(type)) {
        operation_manager_notify_custom(type, OP_STATE_ERROR, "invalid op");
        return OP_START_INVALID;
    }
    if (g_op_cmd_q == NULL) {
        operation_manager_notify_custom(type, OP_STATE_ERROR, "queue");
        return OP_START_FAILED;
    }
    op_status_cache_t snap;
    operation_manager_cache_read(&snap);
    if (snap.state == OP_STATE_IN_SERVICE) {
        operation_manager_notify_custom(type, OP_STATE_ERROR, "busy");
        return OP_START_BUSY;
    }
    op_cmd_t cmd = {.type = type};
    if (xQueueSend(g_op_cmd_q, &cmd, 0) != pdTRUE) {
        operation_manager_notify_custom(type, OP_STATE_ERROR, "busy");
        return OP_START_BUSY;
    }
    return OP_START_OK;
}

void operation_manager_finish_success(operation_type_t type) {
    bool should_notify = false;
    bool clear_override = false;
    bool call_finish = false;
    if (g_active == type && g_state == OP_STATE_IN_SERVICE) {
        g_state = OP_STATE_DONE;
        operation_manager_clear_error_locked();
        clear_override = operation_manager_needs_override(type);
        call_finish = true;
        should_notify = true;
    }
    if (call_finish) {
        const op_def_t *def = operation_manager_get_def(type);
        if (def && def->finish) {
            def->finish();
        }
    }
    if (clear_override) {
        fan_control_override_clear((uint8_t)type);
    }
    if (should_notify) {
        operation_manager_notify_state(OP_STATE_DONE);
    }
}

void operation_manager_finish_error(operation_type_t type, const char *err_text) {
    bool should_notify = false;
    bool clear_override = false;
    bool call_finish = false;
    if (g_active == type && g_state == OP_STATE_IN_SERVICE) {
        g_state = OP_STATE_ERROR;
        operation_manager_set_error_locked(err_text);
        clear_override = operation_manager_needs_override(type);
        call_finish = true;
        should_notify = true;
    }
    if (call_finish) {
        const op_def_t *def = operation_manager_get_def(type);
        if (def && def->finish) {
            def->finish();
        }
    }
    if (clear_override) {
        fan_control_override_clear((uint8_t)type);
    }
    if (should_notify) {
        operation_manager_notify_state(OP_STATE_ERROR);
    }
}

void operation_manager_tick(int64_t now_us) {
    (void)now_us;
    if (g_state != OP_STATE_IN_SERVICE) {
        op_cmd_t cmd;
        if (g_op_cmd_q && xQueueReceive(g_op_cmd_q, &cmd, 0) == pdTRUE) {
            const op_def_t *def = operation_manager_get_def(cmd.type);
            if (!def) {
                operation_manager_notify_custom(cmd.type, OP_STATE_ERROR, "invalid op");
                return;
            }
            g_state = OP_STATE_IN_SERVICE;
            g_active = cmd.type;
            operation_manager_clear_error_locked();
            operation_manager_notify_state(OP_STATE_IN_SERVICE);

            const char *err_text = NULL;
            bool started = operation_manager_start_impl(cmd.type, &err_text);
            if (!started) {
                operation_manager_finish_error(cmd.type, err_text ? err_text : "start failed");
                return;
            }
        } else {
            return;
        }
    }

    operation_type_t type = g_active;
    const op_def_t *def = operation_manager_get_def(type);
    if (!def) {
        operation_manager_finish_error(type, "op missing");
        return;
    }

    op_step_result_t res = OP_STEP_DONE;
    const char *err_text = NULL;
    if (def->step) {
        res = def->step(now_us, &err_text);
    }
    if (res == OP_STEP_CONTINUE) {
        return;
    }
    if (res == OP_STEP_ERROR) {
        operation_manager_finish_error(type, err_text ? err_text : "op failed");
        return;
    }
    operation_manager_finish_success(type);
}
