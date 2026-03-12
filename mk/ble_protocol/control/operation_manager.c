#include "operation_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "esp_timer.h"

#include "driver/gpio.h"
#include "fan_control.h"
#include "operation_status_ble.h"
#include "params.h"
#include "state.h"

#define OP_CALIB_GPIO 1
#define OP_DETECT_GPIO 0
#define OP_DETECT_TARGET_TEMP_C 85.0f
#define OP_OPERATION_SLEEP_US (3 * 1000000LL)

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

static bool op_gpio_set_output(uint8_t gpio_num, int level, const char **err_text) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t rc = gpio_config(&io);
    if (rc != ESP_OK) {
        if (err_text) *err_text = "gpio cfg";
        return false;
    }
    rc = gpio_set_level(gpio_num, level);
    if (rc != ESP_OK) {
        if (err_text) *err_text = "gpio set";
        return false;
    }
    return true;
}

static void op_gpio_set_input(uint8_t gpio_num) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
}

static int64_t g_calibration_sleep_until_us = 0;
static int64_t g_detect_sleep_until_us = 0;

static bool op_calibration_start(const char **err_text) {
    if (!op_gpio_set_output(OP_CALIB_GPIO, 0, err_text)) {
        return false;
    }
    g_calibration_sleep_until_us = esp_timer_get_time() + OP_OPERATION_SLEEP_US;
    return true;
}

static op_step_result_t op_calibration_step(int64_t now_us, const char **err_text) {
    (void)err_text;
    if (now_us < g_calibration_sleep_until_us) {
        return OP_STEP_CONTINUE;
    }
    return OP_STEP_DONE;
}

static void op_calibration_finish(void) {
    op_gpio_set_input(OP_CALIB_GPIO);
    g_calibration_sleep_until_us = 0;
}

static bool op_detect_start(const char **err_text) {
    if (!op_gpio_set_output(OP_DETECT_GPIO, 1, err_text)) {
        return false;
    }

    params_t current;
    if (!params_read(&current)) {
        if (err_text) *err_text = "params read";
        return false;
    }
    current.target_temp_c = OP_DETECT_TARGET_TEMP_C;
    if (!params_write(&current, PARAM_MASK_TARGET_TEMP)) {
        if (err_text) *err_text = "params write";
        return false;
    }
    if (params_apply(NULL) != PARAM_STATUS_OK) {
        if (err_text) *err_text = "params apply";
        return false;
    }
    g_detect_sleep_until_us = esp_timer_get_time() + OP_OPERATION_SLEEP_US;
    return true;
}

static op_step_result_t op_detect_step(int64_t now_us, const char **err_text) {
    (void)err_text;
    if (now_us < g_detect_sleep_until_us) {
        return OP_STEP_CONTINUE;
    }
    return OP_STEP_DONE;
}

static void op_detect_finish(void) {
    op_gpio_set_input(OP_DETECT_GPIO);
    g_detect_sleep_until_us = 0;
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
    {
        .type = OP_TYPE_FAN_CONTROL_DETECT,
        .guard = {
            .uses_override = true,
            .override_rpm = 0.0f,
        },
        .start = op_detect_start,
        .step = op_detect_step,
        .finish = op_detect_finish,
    },
};

static portMUX_TYPE g_op_mux = portMUX_INITIALIZER_UNLOCKED;
static operation_state_t g_state = OP_STATE_IDLE;
static operation_type_t g_active = OP_TYPE_NONE;
static char g_error_text[OP_ERROR_TEXT_MAX + 1];
static uint8_t g_error_len = 0;

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

    if (state == OP_STATE_ERROR && err_text) {
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

    uint16_t conn = fsm_get_conn_handle();
    operation_status_notify(conn, payload, sizeof(payload));
}

static void operation_manager_notify_state(operation_state_t state_override) {
    operation_type_t type;
    char err_buf[OP_ERROR_TEXT_MAX + 1];

    portENTER_CRITICAL(&g_op_mux);
    type = g_active;
    if (state_override == OP_STATE_ERROR && g_error_len > 0) {
        memcpy(err_buf, g_error_text, g_error_len);
        err_buf[g_error_len] = '\0';
    } else {
        err_buf[0] = '\0';
    }
    portEXIT_CRITICAL(&g_op_mux);

    operation_manager_notify_custom(type, state_override, err_buf[0] ? err_buf : NULL);
}

static bool operation_manager_needs_override(operation_type_t type) {
    const op_def_t *def = operation_manager_get_def(type);
    return def != NULL && def->guard.uses_override;
}

void operation_manager_init(void) {
    portENTER_CRITICAL(&g_op_mux);
    g_state = OP_STATE_IDLE;
    g_active = OP_TYPE_NONE;
    operation_manager_clear_error_locked();
    portEXIT_CRITICAL(&g_op_mux);
}

operation_state_t operation_manager_get_state(void) {
    operation_state_t state;
    portENTER_CRITICAL(&g_op_mux);
    state = g_state;
    portEXIT_CRITICAL(&g_op_mux);
    return state;
}

operation_type_t operation_manager_get_active_type(void) {
    operation_type_t type;
    portENTER_CRITICAL(&g_op_mux);
    type = g_active;
    portEXIT_CRITICAL(&g_op_mux);
    return type;
}

bool operation_manager_is_active(void) {
    bool active;
    portENTER_CRITICAL(&g_op_mux);
    active = (g_state == OP_STATE_IN_SERVICE);
    portEXIT_CRITICAL(&g_op_mux);
    return active;
}

void operation_manager_get_status_payload(uint8_t *out, size_t len) {
    if (!out || len < OP_STATUS_PAYLOAD_LEN) return;
    operation_state_t state;
    operation_type_t type;
    uint8_t err_len = 0;
    char err_buf[OP_ERROR_TEXT_MAX];

    portENTER_CRITICAL(&g_op_mux);
    state = g_state;
    type = g_active;
    if (state == OP_STATE_ERROR && g_error_len > 0) {
        err_len = g_error_len;
        memcpy(err_buf, g_error_text, g_error_len);
    }
    portEXIT_CRITICAL(&g_op_mux);

    out[0] = OP_STATUS_VERSION;
    out[1] = (uint8_t)type;
    out[2] = (uint8_t)state;
    out[3] = err_len;
    memset(out + 4, 0, OP_ERROR_TEXT_MAX);
    if (err_len > 0) {
        memcpy(out + 4, err_buf, err_len);
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

    portENTER_CRITICAL(&g_op_mux);
    if (g_state == OP_STATE_IN_SERVICE) {
        portEXIT_CRITICAL(&g_op_mux);
        operation_manager_notify_custom(type, OP_STATE_ERROR, "busy");
        return OP_START_BUSY;
    }
    g_state = OP_STATE_IN_SERVICE;
    g_active = type;
    operation_manager_clear_error_locked();
    portEXIT_CRITICAL(&g_op_mux);

    operation_manager_notify_state(OP_STATE_IN_SERVICE);

    const char *err_text = NULL;
    bool started = operation_manager_start_impl(type, &err_text);
    if (!started) {
        operation_manager_finish_error(type, err_text ? err_text : "start failed");
        return OP_START_FAILED;
    }
    return OP_START_OK;
}

void operation_manager_finish_success(operation_type_t type) {
    bool should_notify = false;
    bool clear_override = false;
    bool call_finish = false;
    portENTER_CRITICAL(&g_op_mux);
    if (g_active == type && g_state == OP_STATE_IN_SERVICE) {
        g_state = OP_STATE_DONE;
        operation_manager_clear_error_locked();
        clear_override = operation_manager_needs_override(type);
        call_finish = true;
        should_notify = true;
    }
    portEXIT_CRITICAL(&g_op_mux);
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
    portENTER_CRITICAL(&g_op_mux);
    if (g_active == type && g_state == OP_STATE_IN_SERVICE) {
        g_state = OP_STATE_ERROR;
        operation_manager_set_error_locked(err_text);
        clear_override = operation_manager_needs_override(type);
        call_finish = true;
        should_notify = true;
    }
    portEXIT_CRITICAL(&g_op_mux);
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
    operation_type_t type = OP_TYPE_NONE;
    const op_def_t *def = NULL;
    portENTER_CRITICAL(&g_op_mux);
    if (g_state == OP_STATE_IN_SERVICE) {
        type = g_active;
        def = operation_manager_get_def(type);
    }
    portEXIT_CRITICAL(&g_op_mux);

    if (!def) {
        if (type != OP_TYPE_NONE) {
            operation_manager_finish_error(type, "op missing");
        }
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
