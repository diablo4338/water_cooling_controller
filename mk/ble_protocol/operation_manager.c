#include "operation_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "fan_control.h"
#include "operation_status_ble.h"
#include "state.h"

static portMUX_TYPE g_op_mux = portMUX_INITIALIZER_UNLOCKED;
static operation_state_t g_state = OP_STATE_IDLE;
static operation_type_t g_active = OP_TYPE_NONE;
static char g_error_text[OP_ERROR_TEXT_MAX + 1];
static uint8_t g_error_len = 0;

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
    active = (g_state == OP_STATE_IN_WORK);
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

static bool operation_manager_start_impl(operation_type_t type) {
    switch (type) {
        case OP_TYPE_FAN_CALIBRATION:
            return fan_control_start_calibration();
        case OP_TYPE_NONE:
        default:
            return false;
    }
}

operation_start_result_t operation_manager_start(operation_type_t type) {
    if (type != OP_TYPE_FAN_CALIBRATION) {
        portENTER_CRITICAL(&g_op_mux);
        g_state = OP_STATE_ERROR;
        g_active = OP_TYPE_NONE;
        operation_manager_set_error_locked("invalid op");
        portEXIT_CRITICAL(&g_op_mux);
        operation_manager_notify_state(OP_STATE_ERROR);
        return OP_START_INVALID;
    }

    portENTER_CRITICAL(&g_op_mux);
    if (g_state == OP_STATE_IN_WORK) {
        portEXIT_CRITICAL(&g_op_mux);
        operation_manager_notify_custom(type, OP_STATE_ERROR, "busy");
        return OP_START_BUSY;
    }
    g_state = OP_STATE_IN_WORK;
    g_active = type;
    operation_manager_clear_error_locked();
    portEXIT_CRITICAL(&g_op_mux);

    operation_manager_notify_state(OP_STATE_IN_WORK);

    bool started = operation_manager_start_impl(type);
    if (!started) {
        operation_manager_finish_error(type, "start failed");
        return OP_START_FAILED;
    }
    return OP_START_OK;
}

void operation_manager_finish_success(operation_type_t type) {
    bool should_notify = false;
    portENTER_CRITICAL(&g_op_mux);
    if (g_active == type && g_state == OP_STATE_IN_WORK) {
        g_state = OP_STATE_DONE;
        operation_manager_clear_error_locked();
        should_notify = true;
    }
    portEXIT_CRITICAL(&g_op_mux);
    if (should_notify) {
        operation_manager_notify_state(OP_STATE_DONE);
    }
}

void operation_manager_finish_error(operation_type_t type, const char *err_text) {
    bool should_notify = false;
    portENTER_CRITICAL(&g_op_mux);
    if (g_active == type && g_state == OP_STATE_IN_WORK) {
        g_state = OP_STATE_ERROR;
        operation_manager_set_error_locked(err_text);
        should_notify = true;
    }
    portEXIT_CRITICAL(&g_op_mux);
    if (should_notify) {
        operation_manager_notify_state(OP_STATE_ERROR);
    }
}
