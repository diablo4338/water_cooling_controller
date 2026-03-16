#include "fan_control.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ledc.h"
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
#define FAN_PWM_GPIO_DC 5
#define FAN_PWM_GPIO_PWM 4
#define FAN_PWM_FREQ_HZ 25000
#define FAN_PWM_RES_BITS LEDC_TIMER_10_BIT
#define FAN_PWM_MAX_DUTY ((1U << 10) - 1U)
static const ledc_mode_t FAN_PWM_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_channel_t FAN_PWM_CHANNEL = LEDC_CHANNEL_0;
static const ledc_timer_t FAN_PWM_TIMER = LEDC_TIMER_0;
static portMUX_TYPE g_fan_mux = portMUX_INITIALIZER_UNLOCKED;
static fan_state_t g_state = FAN_STATE_IDLE;
static int64_t g_state_enter_us = 0;
static int64_t g_last_rpm_ok_us = 0;
static fan_state_t g_last_reported = FAN_STATE_IDLE;
static operation_type_t g_last_reported_op = OP_TYPE_NONE;
static bool g_override_active = false;
static operation_type_t g_override_op = OP_TYPE_NONE;
static float g_override_rpm = 0.0f;

static float fan_control_regulate(const params_t *params, float temp_c, float rpm) {
    (void)rpm;
    if (!params) return 0.0f;
    if (!isfinite(temp_c)) {
        return 0.0f;
    }
    if (temp_c > 15.0f) {
        return (float)params->fan_min_speed;
    }
    return 0.0f;
}

static void fan_control_apply_output(uint8_t control_type, float target_percent) {
    static float last_target = -1.0f;
    static uint8_t last_type = 0xFF;
    static int last_gpio = -1;
    static bool pwm_ready = false;

    if (target_percent < 0.0f) target_percent = 0.0f;
    if (target_percent > 100.0f) target_percent = 100.0f;

    int gpio = (control_type == PARAM_FAN_CONTROL_PWM) ? FAN_PWM_GPIO_PWM : FAN_PWM_GPIO_DC;
    if (!pwm_ready) {
        ledc_timer_config_t timer = {
            .speed_mode = FAN_PWM_MODE,
            .duty_resolution = FAN_PWM_RES_BITS,
            .timer_num = FAN_PWM_TIMER,
            .freq_hz = FAN_PWM_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&timer) == ESP_OK) {
            pwm_ready = true;
        }
    }

    if (fabsf(target_percent - last_target) < 0.5f && last_type == control_type && last_gpio == gpio) {
        return;
    }

    if (pwm_ready && (last_gpio != gpio || last_type != control_type)) {
        ledc_channel_config_t ch = {
            .gpio_num = gpio,
            .speed_mode = FAN_PWM_MODE,
            .channel = FAN_PWM_CHANNEL,
            .timer_sel = FAN_PWM_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&ch);
    }

    uint32_t duty = (uint32_t)lroundf((target_percent / 100.0f) * (float)FAN_PWM_MAX_DUTY);
    if (pwm_ready) {
        ledc_set_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL, duty);
        ledc_update_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL);
    }

    last_target = target_percent;
    last_type = control_type;
    last_gpio = gpio;
    ESP_LOGI(FAN_TAG, "Fan target=%.1f%% type=%s gpio=%d",
             (double)target_percent,
             control_type == PARAM_FAN_CONTROL_PWM ? "PWM" : "DC",
             gpio);
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

bool fan_control_override_set(uint8_t op_type, float target_rpm) {
    bool ok = false;
    portENTER_CRITICAL(&g_fan_mux);
    if (!g_override_active) {
        g_override_active = true;
        g_override_op = (operation_type_t)op_type;
        g_override_rpm = target_rpm;
        ok = true;
    }
    portEXIT_CRITICAL(&g_fan_mux);
    return ok;
}

void fan_control_override_clear(uint8_t op_type) {
    portENTER_CRITICAL(&g_fan_mux);
    if (g_override_active && g_override_op == (operation_type_t)op_type) {
        g_override_active = false;
        g_override_op = OP_TYPE_NONE;
        g_override_rpm = 0.0f;
    }
    portEXIT_CRITICAL(&g_fan_mux);
}

static operation_type_t fan_control_current_operation(void) {
    if (!operation_manager_is_active()) {
        return OP_TYPE_NONE;
    }
    return operation_manager_get_active_type();
}

static bool fan_control_override_get(operation_type_t *op_type, float *rpm) {
    bool active = false;
    portENTER_CRITICAL(&g_fan_mux);
    active = g_override_active;
    if (active) {
        if (op_type) *op_type = g_override_op;
        if (rpm) *rpm = g_override_rpm;
    }
    portEXIT_CRITICAL(&g_fan_mux);
    return active;
}

static void fan_control_notify_state(fan_state_t state) {
    operation_type_t op = fan_control_current_operation();
    if (state == g_last_reported && op == g_last_reported_op) {
        return;
    }
    g_last_reported = state;
    g_last_reported_op = op;
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
    g_last_reported_op = OP_TYPE_NONE;
    g_override_active = false;
    g_override_op = OP_TYPE_NONE;
    g_override_rpm = 0.0f;
    portEXIT_CRITICAL(&g_fan_mux);
}

void fan_control_task(void *param) {
    (void)param;
    const TickType_t delay = pdMS_TO_TICKS(200);

    while (1) {
        int64_t now = esp_timer_get_time();
        operation_manager_tick(now);

        fan_state_t state = fan_control_get_state();
        bool op_active = operation_manager_is_active();
        operation_type_t op_type = op_active ? operation_manager_get_active_type() : OP_TYPE_NONE;
        operation_type_t override_op = OP_TYPE_NONE;
        float override_rpm = 0.0f;
        bool override_active = fan_control_override_get(&override_op, &override_rpm);

        if (op_active) {
            if (state != FAN_STATE_IN_SERVICE) {
                fan_control_set_state(FAN_STATE_IN_SERVICE, now);
                fan_control_notify_state(FAN_STATE_IN_SERVICE);
            }
            params_t params;
            uint8_t control_type = PARAM_FAN_CONTROL_DC;
            if (params_cache_get(&params)) {
                control_type = params.fan_control_type;
            }
            if (override_active && override_op == op_type) {
                fan_control_apply_output(control_type, override_rpm);
            } else {
                fan_control_apply_output(control_type, 0.0f);
            }
            vTaskDelay(delay);
            continue;
        }

        if (state == FAN_STATE_IN_SERVICE) {
            fan_control_set_state(FAN_STATE_IDLE, now);
            fan_control_notify_state(FAN_STATE_IDLE);
            state = FAN_STATE_IDLE;
        }

        params_t params;
        if (!params_cache_get(&params)) {
            vTaskDelay(delay);
            continue;
        }

        float rpm = metrics_get_fan_speed_rpm();
        float temp = metrics_get_temp(3);
        float target_percent = fan_control_regulate(&params, temp, rpm);
        bool command_on = target_percent > 0.0f;
        bool rpm_ok = isfinite(rpm) && rpm >= FAN_RPM_THRESHOLD;

        fan_control_apply_output(params.fan_control_type, target_percent);

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
