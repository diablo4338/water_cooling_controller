#include "operation_preflight.h"

#include <math.h>
#include <stddef.h>

#include "esp_log.h"

#include "fan_control.h"
#include "output_limit.h"
#include "params.h"

#define PREFLIGHT_CONFIRM_US (3000 * 1000LL)
// Bound the complete search, including the final minimum-output check.
#define PREFLIGHT_TIMEOUT_US (45000 * 1000LL)

static const char *TAG = "op-preflight";

typedef enum {
    PREFLIGHT_PHASE_IDLE = 0,
    PREFLIGHT_PHASE_TEST,
} preflight_phase_t;

typedef enum {
    PREFLIGHT_EVENT_TEST_STARTED,
    PREFLIGHT_EVENT_TEST_PASSED,
    PREFLIGHT_EVENT_RESET,
} preflight_event_t;

static preflight_phase_t g_phase = PREFLIGHT_PHASE_IDLE;
static operation_type_t g_type = OP_TYPE_NONE;
static uint8_t g_tests[2] = {0};
static uint8_t g_test_count = 0;
static uint8_t g_test_index = 0;
static int64_t g_confirm_at_us = 0;
static int64_t g_timeout_at_us = 0;
static operation_preflight_caps_t g_caps = {
    .dc_max_percent = 100.0f,
    .pwm_max_percent = 100.0f,
};

static void preflight_dispatch(preflight_event_t event) {
    switch (event) {
        case PREFLIGHT_EVENT_TEST_STARTED:
            if (g_test_index < g_test_count) g_phase = PREFLIGHT_PHASE_TEST;
            break;
        case PREFLIGHT_EVENT_TEST_PASSED:
            if (g_phase != PREFLIGHT_PHASE_TEST) break;
            g_test_index++;
            if (g_test_index == g_test_count) {
                g_phase = PREFLIGHT_PHASE_IDLE;
                g_type = OP_TYPE_NONE;
            }
            break;
        case PREFLIGHT_EVENT_RESET:
            g_phase = PREFLIGHT_PHASE_IDLE;
            g_type = OP_TYPE_NONE;
            break;
    }
}

static float preflight_clamp_percent(float value) {
    if (!isfinite(value)) {
        return 100.0f;
    }
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 100.0f) {
        return 100.0f;
    }
    return value;
}

static void preflight_store_cap(uint8_t control_type, float cap) {
    cap = preflight_clamp_percent(cap);
    if (control_type == PARAM_FAN_CONTROL_PWM) {
        g_caps.pwm_max_percent = cap;
    } else {
        g_caps.dc_max_percent = cap;
    }
}

static bool preflight_add_test(uint8_t control_type) {
    if (control_type != PARAM_FAN_CONTROL_DC && control_type != PARAM_FAN_CONTROL_PWM) {
        return false;
    }
    for (uint8_t idx = 0; idx < g_test_count; idx++) {
        if (g_tests[idx] == control_type) {
            return true;
        }
    }
    if (g_test_count >= sizeof(g_tests)) {
        return false;
    }
    g_tests[g_test_count++] = control_type;
    return true;
}

static bool preflight_select_tests(operation_type_t type, const char **err_text) {
    g_test_count = 0;
    if (type == OP_TYPE_SETUP_FANS) {
        return preflight_add_test(PARAM_FAN_CONTROL_DC) &&
               preflight_add_test(PARAM_FAN_CONTROL_PWM);
    }

    params_t current;
    if (!params_read(&current)) {
        if (err_text) {
            *err_text = "params read";
        }
        return false;
    }
    return preflight_add_test(current.fan_control_type);
}

static bool preflight_start_current_test(int64_t now_us, const char **err_text) {
    if (g_test_index >= g_test_count) {
        return false;
    }
    uint8_t control_type = g_tests[g_test_index];
    if (!fan_control_override_set_output((uint8_t)g_type, control_type, 100.0f)) {
        if (err_text) {
            *err_text = "fan busy";
        }
        return false;
    }
    g_confirm_at_us = now_us + PREFLIGHT_CONFIRM_US;
    g_timeout_at_us = now_us + PREFLIGHT_TIMEOUT_US;
    preflight_dispatch(PREFLIGHT_EVENT_TEST_STARTED);
    ESP_LOGI(TAG, "start type=%u control=%u", (unsigned)g_type, (unsigned)control_type);
    return true;
}

void operation_preflight_reset(void) {
    if (g_type != OP_TYPE_NONE) {
        fan_control_override_clear((uint8_t)g_type);
    }
    preflight_dispatch(PREFLIGHT_EVENT_RESET);
    g_test_count = 0;
    g_test_index = 0;
    g_confirm_at_us = 0;
    g_timeout_at_us = 0;
    g_caps.dc_max_percent = 100.0f;
    g_caps.pwm_max_percent = 100.0f;
}

bool operation_preflight_start(operation_type_t type, int64_t now_us, const char **err_text) {
    operation_preflight_reset();
    g_type = type;
    if (err_text) {
        *err_text = NULL;
    }
    if (params_get_output_max() == 0) {
        if (err_text) *err_text = "saved output limit unavailable";
        operation_preflight_reset();
        return false;
    }
    if (!preflight_select_tests(type, err_text)) {
        operation_preflight_reset();
        return false;
    }
    return preflight_start_current_test(now_us, err_text);
}

bool operation_preflight_is_active(void) {
    return g_phase != PREFLIGHT_PHASE_IDLE;
}

operation_preflight_result_t operation_preflight_step(int64_t now_us, const char **err_text) {
    if (err_text) {
        *err_text = NULL;
    }
    if (g_phase == PREFLIGHT_PHASE_IDLE) {
        return OP_PREFLIGHT_RESULT_DONE;
    }
    if (g_phase != PREFLIGHT_PHASE_TEST || g_test_index >= g_test_count) {
        if (err_text) {
            *err_text = "preflight state";
        }
        operation_preflight_reset();
        return OP_PREFLIGHT_RESULT_ERROR;
    }
    if (now_us < g_confirm_at_us) {
        return OP_PREFLIGHT_RESULT_CONTINUE;
    }
    output_limit_status_t limit = output_limit_get_status();
    if (limit.state == OUTPUT_LIMIT_FAULT ||
        limit.state == OUTPUT_LIMIT_VERIFY_TIMEOUT || limit.state == OUTPUT_LIMIT_MINIMUM ||
        limit.state == OUTPUT_LIMIT_START_BLOCKED || limit.effective == 0) {
        if (err_text) *err_text = "output limit alarm";
        return OP_PREFLIGHT_RESULT_ERROR;
    }
    if (!limit.verified_output ||
        (limit.state != OUTPUT_LIMIT_NORMAL && limit.state != OUTPUT_LIMIT_LIMITED)) {
        if (now_us >= g_timeout_at_us) {
            if (err_text) *err_text = "preflight timeout";
            return OP_PREFLIGHT_RESULT_ERROR;
        }
        return OP_PREFLIGHT_RESULT_CONTINUE;
    }

    uint8_t control_type = g_tests[g_test_index];
    float cap = limit.effective;
    preflight_store_cap(control_type, cap);
    ESP_LOGI(TAG, "done type=%u control=%u cap=%.1f%%",
             (unsigned)g_type, (unsigned)control_type, (double)cap);

    fan_control_override_clear((uint8_t)g_type);
    preflight_dispatch(PREFLIGHT_EVENT_TEST_PASSED);

    if (g_test_index < g_test_count) {
        return preflight_start_current_test(now_us, err_text)
                   ? OP_PREFLIGHT_RESULT_CONTINUE
                   : OP_PREFLIGHT_RESULT_ERROR;
    }

    return OP_PREFLIGHT_RESULT_DONE;
}

bool operation_preflight_get_caps(operation_preflight_caps_t *out) {
    if (!out) {
        return false;
    }
    *out = g_caps;
    return true;
}
