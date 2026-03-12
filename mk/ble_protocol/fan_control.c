#include "fan_control.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fan_status_ble.h"
#include "metrics.h"
#include "operation_manager.h"
#include "params.h"
#include "state.h"

#define FAN_TAG "fan-ctl"

#define FAN_RPM_THRESHOLD 300.0f
#define FAN_START_TIMEOUT_US 500000
#define FAN_FAIL_TIMEOUT_US 1000000
#define FAN_CALIBRATION_RPM 2500.0f
#define FAN_CALIBRATION_TIME_US (5 * 1000000LL)

static portMUX_TYPE g_fan_mux = portMUX_INITIALIZER_UNLOCKED;
static fan_state_t g_state = FAN_STATE_IDLE;
static int64_t g_state_enter_us = 0;
static int64_t g_last_rpm_ok_us = 0;
static fan_state_t g_last_reported = FAN_STATE_IDLE;
static bool g_calibrating = false;
static int64_t g_calibrate_until_us = 0;

static float fan_control_regulate(const params_t *params, float temp_c, float rpm) {
    (void)rpm;
    if (!params) return 0.0f;
    if (!isfinite(temp_c)) {
        return 0.0f;
    }
    if (temp_c > 15.0f) {
        return params->fan_min_rpm;
    }
    return 0.0f;
}

static void fan_control_apply_output(float target_rpm) {
    static float last_target = -1.0f;
    if (fabsf(target_rpm - last_target) < 1.0f) {
        return;
    }
    last_target = target_rpm;
    ESP_LOGI(FAN_TAG, "Fan target rpm=%.1f (stub)", (double)target_rpm);
}

static void fan_control_set_state(fan_state_t next, int64_t now_us) {
    portENTER_CRITICAL(&g_fan_mux);
    g_state = next;
    g_state_enter_us = now_us;
    if (next == FAN_STATE_RUNNING) {
        g_last_rpm_ok_us = now_us;
    }
    portEXIT_CRITICAL(&g_fan_mux);
}

static fan_state_t fan_control_get_state(void) {
    fan_state_t state;
    portENTER_CRITICAL(&g_fan_mux);
    state = g_state;
    portEXIT_CRITICAL(&g_fan_mux);
    return state;
}

bool fan_control_is_calibrating(void) {
    bool value;
    portENTER_CRITICAL(&g_fan_mux);
    value = g_calibrating;
    portEXIT_CRITICAL(&g_fan_mux);
    return value;
}

bool fan_control_start_calibration(void) {
    bool ok = false;
    portENTER_CRITICAL(&g_fan_mux);
    if (!g_calibrating) {
        g_calibrating = true;
        g_calibrate_until_us = esp_timer_get_time() + FAN_CALIBRATION_TIME_US;
        ok = true;
    }
    portEXIT_CRITICAL(&g_fan_mux);
    return ok;
}

static bool fan_control_calibration_update(int64_t now_us) {
    bool calibrating;
    bool done = false;
    portENTER_CRITICAL(&g_fan_mux);
    if (g_calibrating && now_us >= g_calibrate_until_us) {
        g_calibrating = false;
        done = true;
    }
    calibrating = g_calibrating;
    portEXIT_CRITICAL(&g_fan_mux);
    if (done) {
        operation_manager_finish_success(OP_TYPE_FAN_CALIBRATION);
    }
    return calibrating;
}

static operation_type_t fan_control_current_operation(void) {
    if (!operation_manager_is_active()) {
        return OP_TYPE_NONE;
    }
    return operation_manager_get_active_type();
}

static void fan_control_notify_state(fan_state_t state) {
    if (state == g_last_reported) {
        return;
    }
    g_last_reported = state;
    operation_type_t op = fan_control_current_operation();
    uint8_t payload[FAN_STATUS_PAYLOAD_LEN] = {
        FAN_STATUS_VERSION,
        (uint8_t)state,
        (uint8_t)op,
    };
    uint16_t conn = fsm_get_conn_handle();
    fan_status_notify(conn, payload, sizeof(payload));
}

void fan_control_get_status_payload(uint8_t *out, size_t len) {
    if (!out || len < FAN_STATUS_PAYLOAD_LEN) return;
    fan_state_t state = fan_control_get_state();
    operation_type_t op = fan_control_current_operation();
    out[0] = FAN_STATUS_VERSION;
    out[1] = (uint8_t)state;
    out[2] = (uint8_t)op;
}

void fan_control_init(void) {
    portENTER_CRITICAL(&g_fan_mux);
    g_state = FAN_STATE_IDLE;
    g_state_enter_us = esp_timer_get_time();
    g_last_rpm_ok_us = 0;
    g_last_reported = FAN_STATE_IDLE;
    g_calibrating = false;
    g_calibrate_until_us = 0;
    portEXIT_CRITICAL(&g_fan_mux);
}

void fan_control_task(void *param) {
    (void)param;
    const TickType_t delay = pdMS_TO_TICKS(200);

    while (1) {
        int64_t now = esp_timer_get_time();

        params_t params;
        if (!params_read(&params)) {
            vTaskDelay(delay);
            continue;
        }

        float rpm = metrics_get_fan_speed_rpm();
        float temp = metrics_get_temp(3);
        float target_rpm = fan_control_regulate(&params, temp, rpm);
        bool command_on = target_rpm > 0.0f;
        bool rpm_ok = isfinite(rpm) && rpm >= FAN_RPM_THRESHOLD;

        fan_state_t state = fan_control_get_state();
        bool calibrating = fan_control_calibration_update(now);
        bool op_active = operation_manager_is_active();
        operation_type_t op_type = op_active ? operation_manager_get_active_type() : OP_TYPE_NONE;

        if (op_active) {
            if (op_type == OP_TYPE_FAN_CALIBRATION && calibrating) {
                if (state != FAN_STATE_IN_SERVICE) {
                    fan_control_set_state(FAN_STATE_IN_SERVICE, now);
                    fan_control_notify_state(FAN_STATE_IN_SERVICE);
                }
                fan_control_apply_output(FAN_CALIBRATION_RPM);
                vTaskDelay(delay);
                continue;
            }

            if (state != FAN_STATE_IN_SERVICE) {
                fan_control_set_state(FAN_STATE_IN_SERVICE, now);
                fan_control_notify_state(FAN_STATE_IN_SERVICE);
            }
            fan_control_apply_output(0.0f);
            vTaskDelay(delay);
            continue;
        }

        if (state == FAN_STATE_IN_SERVICE) {
            fan_control_set_state(FAN_STATE_IDLE, now);
            fan_control_notify_state(FAN_STATE_IDLE);
            state = FAN_STATE_IDLE;
        }

        fan_control_apply_output(target_rpm);

        if (!command_on) {
            if (state != FAN_STATE_IDLE) {
                fan_control_set_state(FAN_STATE_IDLE, now);
                fan_control_notify_state(FAN_STATE_IDLE);
            }
            vTaskDelay(delay);
            continue;
        }

        if (rpm_ok) {
            portENTER_CRITICAL(&g_fan_mux);
            g_last_rpm_ok_us = now;
            portEXIT_CRITICAL(&g_fan_mux);
        }

        switch (state) {
            case FAN_STATE_IDLE:
                fan_control_set_state(FAN_STATE_STARTING, now);
                break;
            case FAN_STATE_STARTING:
                if (rpm_ok) {
                    fan_control_set_state(FAN_STATE_RUNNING, now);
                    fan_control_notify_state(FAN_STATE_RUNNING);
                } else if (now - g_state_enter_us >= FAN_START_TIMEOUT_US) {
                    ESP_LOGE(FAN_TAG, "Fan stall detected (start timeout), rpm=%.1f", (double)rpm);
                    fan_control_set_state(FAN_STATE_STALL, now);
                    fan_control_notify_state(FAN_STATE_STALL);
                }
                break;
            case FAN_STATE_RUNNING:
                if (!rpm_ok && now - g_last_rpm_ok_us >= FAN_FAIL_TIMEOUT_US) {
                    ESP_LOGE(FAN_TAG, "Fan stall detected (run timeout), rpm=%.1f", (double)rpm);
                    fan_control_set_state(FAN_STATE_STALL, now);
                    fan_control_notify_state(FAN_STATE_STALL);
                } else if (rpm_ok) {
                    fan_control_notify_state(FAN_STATE_RUNNING);
                }
                break;
            case FAN_STATE_STALL:
                if (rpm_ok) {
                    fan_control_set_state(FAN_STATE_RUNNING, now);
                    fan_control_notify_state(FAN_STATE_RUNNING);
                }
                break;
            default:
                fan_control_set_state(FAN_STATE_IDLE, now);
                break;
        }

        vTaskDelay(delay);
    }
}
