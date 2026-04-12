#include "fan_control.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
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
#define FAN_HIGHSIDE_GPIO 3
#define FAN_PWM_GPIO_DC 10
#define FAN_PWM_GPIO_PWM 2
#define FAN_PWM_FREQ_HZ 25000
#define FAN_DC_FREQ_HZ 20000
#define FAN_PWM_RES_BITS LEDC_TIMER_10_BIT
#define FAN_DC_RES_BITS LEDC_TIMER_10_BIT
static const ledc_mode_t FAN_PWM_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_channel_t FAN_PWM_CHANNEL_ACTIVE = LEDC_CHANNEL_0;
static const ledc_timer_t FAN_PWM_TIMER = LEDC_TIMER_0;

static uint32_t fan_control_max_duty(ledc_timer_bit_t res_bits) {
    return (1U << (uint32_t)res_bits) - 1U;
}
static operation_type_t fan_control_current_operation(void);
static fan_state_t g_state = FAN_STATE_IDLE;
static int64_t g_state_enter_us = 0;
static int64_t g_last_rpm_ok_us = 0;
static fan_state_t g_last_reported = FAN_STATE_IDLE;
static operation_type_t g_last_reported_op = OP_TYPE_NONE;
static bool g_override_active = false;
static operation_type_t g_override_op = OP_TYPE_NONE;
static float g_override_rpm = 0.0f;
static uint32_t g_status_seq = 0;
typedef struct {
    fan_state_t state;
    operation_type_t op;
} fan_status_cache_t;
static fan_status_cache_t g_status_cache = {0};

static void fan_status_cache_write(fan_state_t state, operation_type_t op) {
    fan_status_cache_t snap = {
        .state = state,
        .op = op,
    };
    __atomic_fetch_add(&g_status_seq, 1U, __ATOMIC_ACQ_REL);
    g_status_cache = snap;
    __atomic_fetch_add(&g_status_seq, 1U, __ATOMIC_ACQ_REL);
}

static void fan_status_cache_read(fan_status_cache_t *out) {
    if (!out) return;
    while (1) {
        uint32_t seq1 = __atomic_load_n(&g_status_seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1U) {
            continue;
        }
        fan_status_cache_t snap = g_status_cache;
        uint32_t seq2 = __atomic_load_n(&g_status_seq, __ATOMIC_ACQUIRE);
        if (seq1 == seq2) {
            *out = snap;
            return;
        }
    }
}

static bool fan_control_inverted(uint8_t control_type) {
    return control_type == PARAM_FAN_CONTROL_PWM;
}

static float fan_control_regulate(const params_t *params, float temp_c, float rpm) {
    (void)rpm;
    static bool temp_mode_running = false;
    if (!params) return 0.0f;
    if (!params->fan_active) {
        temp_mode_running = false;
        return 0.0f;
    }

    if (params->fan_mode == PARAM_FAN_MODE_CONTINUOUS) {
        temp_mode_running = false;
        return (float)params->fan_min_speed;
    }

    if (!isfinite(temp_c)) {
        return temp_mode_running ? (float)params->fan_min_speed : 0.0f;
    }

    float start_temp = (float)params->fan_start_temp;
    float off_temp = start_temp - (float)params->fan_off_delta;
    float max_temp = (float)params->fan_max_temp;
    if (!temp_mode_running) {
        if (temp_c >= start_temp) {
            temp_mode_running = true;
        } else {
            return 0.0f;
        }
    } else if (temp_c <= off_temp) {
        temp_mode_running = false;
        return 0.0f;
    }

    if (temp_c >= max_temp) {
        return 100.0f;
    }

    if (max_temp <= start_temp) {
        return (float)params->fan_min_speed;
    }

    float ratio = (temp_c - start_temp) / (max_temp - start_temp);
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return (float)params->fan_min_speed + ratio * (100.0f - (float)params->fan_min_speed);
}

static void fan_control_apply_output(uint8_t control_type, float target_percent) {
    static float last_target = -1.0f;
    static uint8_t last_type = 0xFF;
    static int last_gpio = -1;
    static bool highside_ready = false;
    static int last_highside = -1;
    static int last_other_gpio = -1;
    static bool pwm_ready = false;
    static uint32_t last_freq_hz = 0;
    static ledc_timer_bit_t last_res_bits = LEDC_TIMER_10_BIT;

    if (target_percent < 0.0f) target_percent = 0.0f;
    if (target_percent > 100.0f) target_percent = 100.0f;

    int highside_level = (target_percent > 0.0f) ? 1 : 0;
    if (!highside_ready) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << FAN_HIGHSIDE_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) == ESP_OK) {
            highside_ready = true;
            last_highside = -1;
        }
    }
    if (highside_ready && last_highside != highside_level) {
        gpio_set_level(FAN_HIGHSIDE_GPIO, highside_level);
        last_highside = highside_level;
    }

    int gpio = (control_type == PARAM_FAN_CONTROL_PWM) ? FAN_PWM_GPIO_PWM : FAN_PWM_GPIO_DC;
    int other_gpio = (control_type == PARAM_FAN_CONTROL_PWM) ? FAN_PWM_GPIO_DC : FAN_PWM_GPIO_PWM;
    uint32_t desired_freq_hz = (control_type == PARAM_FAN_CONTROL_PWM) ? FAN_PWM_FREQ_HZ : FAN_DC_FREQ_HZ;
    ledc_timer_bit_t desired_res_bits = (control_type == PARAM_FAN_CONTROL_PWM) ? FAN_PWM_RES_BITS : FAN_DC_RES_BITS;
    if (!pwm_ready || last_freq_hz != desired_freq_hz || last_res_bits != desired_res_bits) {
        ledc_timer_config_t timer = {
            .speed_mode = FAN_PWM_MODE,
            .duty_resolution = desired_res_bits,
            .timer_num = FAN_PWM_TIMER,
            .freq_hz = desired_freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&timer) == ESP_OK) {
            pwm_ready = true;
            last_freq_hz = desired_freq_hz;
            last_res_bits = desired_res_bits;
        } else {
            pwm_ready = false;
        }
    }

    if (fabsf(target_percent - last_target) < 0.5f && last_type == control_type && last_gpio == gpio) {
        return;
    }

    if (pwm_ready && (last_gpio != gpio || last_type != control_type)) {
        ledc_channel_config_t ch = {
            .gpio_num = gpio,
            .speed_mode = FAN_PWM_MODE,
            .channel = FAN_PWM_CHANNEL_ACTIVE,
            .timer_sel = FAN_PWM_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&ch);
    }

    if (last_other_gpio != other_gpio || last_type != control_type) {
        gpio_config_t other_io = {
            .pin_bit_mask = 1ULL << other_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&other_io) == ESP_OK) {
            gpio_set_level(other_gpio, 1);
        }
    }

    float effective_percent = fan_control_inverted(control_type) ? (100.0f - target_percent) : target_percent;
    uint32_t max_duty = fan_control_max_duty(last_res_bits);
    uint32_t duty = (uint32_t)lroundf((effective_percent / 100.0f) * (float)max_duty);
    if (pwm_ready) {
        ledc_set_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL_ACTIVE, duty);
        ledc_update_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL_ACTIVE);
    }

    last_target = target_percent;
    last_type = control_type;
    last_gpio = gpio;
    last_other_gpio = other_gpio;
    ESP_LOGI(FAN_TAG, "Fan target=%.1f%% (eff=%.1f%%) type=%s gpio=%d",
             (double)target_percent,
             (double)effective_percent,
             control_type == PARAM_FAN_CONTROL_PWM ? "PWM" : "DC",
             gpio);
}

static void fan_control_set_state(fan_state_t next, int64_t now_us) {
    g_state = next;
    g_state_enter_us = now_us;
    if (next == FAN_STATE_RUNNING) {
        g_last_rpm_ok_us = now_us;
    }
    fan_status_cache_write(next, fan_control_current_operation());
}

static fan_state_t fan_control_get_state(void) {
    return g_state;
}

bool fan_control_override_set(uint8_t op_type, float target_rpm) {
    if (!g_override_active || g_override_op == (operation_type_t)op_type) {
        g_override_active = true;
        g_override_op = (operation_type_t)op_type;
        g_override_rpm = target_rpm;
        return true;
    }
    return false;
}

void fan_control_override_clear(uint8_t op_type) {
    if (g_override_active && g_override_op == (operation_type_t)op_type) {
        g_override_active = false;
        g_override_op = OP_TYPE_NONE;
        g_override_rpm = 0.0f;
    }
}

static operation_type_t fan_control_current_operation(void) {
    if (!operation_manager_is_active()) {
        return OP_TYPE_NONE;
    }
    return operation_manager_get_active_type();
}

static bool fan_control_override_get(operation_type_t *op_type, float *rpm) {
    bool active = g_override_active;
    if (active) {
        if (op_type) *op_type = g_override_op;
        if (rpm) *rpm = g_override_rpm;
    }
    return active;
}

static void fan_control_notify_state(fan_state_t state) {
    operation_type_t op = fan_control_current_operation();
    if (state == g_last_reported && op == g_last_reported_op) {
        return;
    }
    g_last_reported = state;
    g_last_reported_op = op;
    fan_status_cache_write(state, op);
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
    fan_status_cache_t snap;
    fan_status_cache_read(&snap);
    out[0] = FAN_STATUS_VERSION;
    out[1] = (uint8_t)snap.state;
    out[2] = (uint8_t)snap.op;
}

void fan_control_init(void) {
    g_state = FAN_STATE_IDLE;
    g_state_enter_us = esp_timer_get_time();
    g_last_rpm_ok_us = 0;
    g_last_reported = FAN_STATE_IDLE;
    g_last_reported_op = OP_TYPE_NONE;
    g_override_active = false;
    g_override_op = OP_TYPE_NONE;
    g_override_rpm = 0.0f;
    fan_status_cache_write(FAN_STATE_IDLE, OP_TYPE_NONE);
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
        bool fan_required = params.fan_active && command_on;
        bool rpm_ok = isfinite(rpm) && rpm >= FAN_RPM_THRESHOLD;

        fan_control_apply_output(params.fan_control_type, target_percent);

        if (!fan_required) {
            if (state != FAN_STATE_IDLE) {
                fan_control_set_state(FAN_STATE_IDLE, now);
                fan_control_notify_state(FAN_STATE_IDLE);
            }
            vTaskDelay(delay);
            continue;
        }

        if (rpm_ok) {
            g_last_rpm_ok_us = now;
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
