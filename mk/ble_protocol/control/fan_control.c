#include "fan_control.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fan_status_ble.h"
#include "device_status.h"
#include "metrics.h"
#include "operation_manager.h"
#include "overcurrent_monitor.h"
#include "params.h"
#include "state.h"

#define FAN_TAG "fan-ctl"

#define FAN_CHANNEL_SCAN_TIMEOUT_US (METRICS_FAN_CHANNELS * 1000000LL)
#define FAN_START_TIMEOUT_US FAN_CHANNEL_SCAN_TIMEOUT_US
#define FAN_FAIL_TIMEOUT_US FAN_CHANNEL_SCAN_TIMEOUT_US
#define FAN_OVERCURRENT_RECOVERY_STEP_PERCENT 3.0f
#define FAN_OVERCURRENT_RECOVERY_WAIT_US (1 * 1000000LL)
#define FAN_HIGHSIDE_GPIO 3
#define FAN_PWM_GPIO_DC 10
#define FAN_PWM_GPIO_PWM 2
#define FAN_PWM_FREQ_HZ 25000
#define FAN_DC_FREQ_HZ 1000
#define FAN_PWM_RES_BITS LEDC_TIMER_10_BIT
#define FAN_DC_RES_BITS LEDC_TIMER_10_BIT
#define FAN_TEMP_CONTROL_CHANNEL 3
static const ledc_mode_t FAN_PWM_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_channel_t FAN_PWM_CHANNEL_ACTIVE = LEDC_CHANNEL_0;
static const ledc_timer_t FAN_PWM_TIMER = LEDC_TIMER_0;

static uint32_t fan_control_max_duty(ledc_timer_bit_t res_bits) {
    return (1U << (uint32_t)res_bits) - 1U;
}
static operation_type_t fan_control_current_operation(void);
static fan_state_t g_state[METRICS_FAN_CHANNELS];
static int64_t g_state_enter_us[METRICS_FAN_CHANNELS];
static int64_t g_last_rpm_ok_us[METRICS_FAN_CHANNELS];
static fan_state_t g_last_reported[METRICS_FAN_CHANNELS];
static operation_type_t g_last_reported_op = OP_TYPE_NONE;
static bool g_override_active = false;
static operation_type_t g_override_op = OP_TYPE_NONE;
static float g_override_rpm = 0.0f;
static bool g_override_has_control_type = false;
static uint8_t g_override_control_type = PARAM_FAN_CONTROL_DC;
static bool g_cycle_active = false;
static bool g_cycle_limit_locked = false;
static float g_cycle_max_percent = 100.0f;
static bool g_recovery_active = false;
static float g_recovery_probe_percent = 100.0f;
static int64_t g_recovery_next_check_us = 0;
static uint32_t g_status_seq = 0;
typedef struct {
    fan_state_t state[METRICS_FAN_CHANNELS];
    operation_type_t op;
} fan_status_cache_t;

static void fan_control_cycle_reset(void) {
    g_cycle_active = false;
    g_cycle_limit_locked = false;
    g_cycle_max_percent = 100.0f;
    g_recovery_active = false;
    g_recovery_probe_percent = 100.0f;
    g_recovery_next_check_us = 0;
}

static void fan_control_cycle_start(void) {
    g_cycle_active = true;
    g_cycle_limit_locked = false;
    g_cycle_max_percent = 100.0f;
    g_recovery_active = false;
    g_recovery_probe_percent = 100.0f;
    g_recovery_next_check_us = 0;
}

static void fan_control_begin_overcurrent_recovery(float applied_percent, int64_t now_us) {
    float next_percent = applied_percent - FAN_OVERCURRENT_RECOVERY_STEP_PERCENT;
    if (next_percent < 0.0f) {
        next_percent = 0.0f;
    }
    g_cycle_active = true;
    g_cycle_limit_locked = false;
    g_recovery_active = true;
    g_cycle_max_percent = next_percent;
    g_recovery_probe_percent = next_percent;
    g_recovery_next_check_us = now_us + FAN_OVERCURRENT_RECOVERY_WAIT_US;
    ESP_LOGW(FAN_TAG, "Overcurrent recovery start: applied=%.1f%% next=%.1f%% wait=%lld ms",
             (double)applied_percent,
             (double)g_recovery_probe_percent,
             FAN_OVERCURRENT_RECOVERY_WAIT_US / 1000LL);
}

static void fan_control_finish_overcurrent_recovery(float locked_percent, bool alert_active) {
    if (locked_percent < 0.0f) {
        locked_percent = 0.0f;
    }
    if (locked_percent > 100.0f) {
        locked_percent = 100.0f;
    }
    g_cycle_max_percent = locked_percent;
    g_cycle_limit_locked = true;
    g_recovery_active = false;
    g_recovery_probe_percent = locked_percent;
    g_recovery_next_check_us = 0;
    overcurrent_monitor_clear_latched();
    ESP_LOGW(FAN_TAG, "Overcurrent recovery locked max=%.1f%% alert=%d",
             (double)g_cycle_max_percent, alert_active ? 1 : 0);
}

static float fan_control_apply_cycle_limit(float requested_percent, int64_t now_us) {
    if (!g_recovery_active && requested_percent <= 0.0f) {
        fan_control_cycle_reset();
        return 0.0f;
    }

    if (!g_cycle_active) {
        fan_control_cycle_start();
    }

    float applied_percent = requested_percent;
    if (g_cycle_limit_locked) {
        applied_percent = fminf(applied_percent, g_cycle_max_percent);
    }

    bool alert_active = overcurrent_monitor_latched_active();
    if (alert_active && !g_recovery_active && applied_percent > 0.0f) {
        ESP_LOGW(FAN_TAG, "Overcurrent latched: requested=%.1f%% applied=%.1f%% locked=%d max=%.1f%%",
                 (double)requested_percent,
                 (double)applied_percent,
                 g_cycle_limit_locked ? 1 : 0,
                 (double)g_cycle_max_percent);
        fan_control_begin_overcurrent_recovery(applied_percent, now_us);
    }

    if (g_recovery_active && now_us >= g_recovery_next_check_us) {
        alert_active = overcurrent_monitor_alert_active();
        ESP_LOGW(FAN_TAG, "Overcurrent recovery check: probe=%.1f%% raw_alert=%d",
                 (double)g_recovery_probe_percent,
                 alert_active ? 1 : 0);
        if (alert_active) {
            uint16_t mask_enable = 0;
            if (overcurrent_monitor_read_alert_status(&mask_enable)) {
                ESP_LOGW(FAN_TAG, "Overcurrent recovery sample: mask=0x%04X", mask_enable);
            } else {
                ESP_LOGW(FAN_TAG, "Overcurrent recovery sample: mask/enable read failed");
            }
            if (g_recovery_probe_percent > 0.0f) {
                g_recovery_probe_percent -= FAN_OVERCURRENT_RECOVERY_STEP_PERCENT;
                if (g_recovery_probe_percent < 0.0f) {
                    g_recovery_probe_percent = 0.0f;
                }
            }
            g_cycle_max_percent = g_recovery_probe_percent;
            g_recovery_next_check_us = now_us + FAN_OVERCURRENT_RECOVERY_WAIT_US;
            ESP_LOGW(FAN_TAG, "Overcurrent recovery backoff=%.1f%% alert=1",
                     (double)g_recovery_probe_percent);
        } else {
            fan_control_finish_overcurrent_recovery(g_recovery_probe_percent, false);
        }
    }

    if (g_recovery_active) {
        return g_recovery_probe_percent;
    }
    if (g_cycle_limit_locked) {
        return fminf(requested_percent, g_cycle_max_percent);
    }
    return requested_percent;
}
static fan_status_cache_t g_status_cache = {0};

static void fan_status_cache_write(operation_type_t op) {
    fan_status_cache_t snap = {
        .op = op,
    };
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        snap.state[ch] = g_state[ch];
    }
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

static float fan_control_regulate(const params_t *params, float temp_c) {
    static bool temp_mode_running = false;
    if (!params) return 0.0f;

    if (params->fan_mode == PARAM_FAN_MODE_INACTIVE) {
        temp_mode_running = false;
        return 0.0f;
    }

    if (params->fan_mode == PARAM_FAN_MODE_CONTINUOUS) {
        temp_mode_running = false;
        return (float)params->fan_min_speed;
    }

    if (params->fan_mode != PARAM_FAN_MODE_TEMP_SENSOR) {
        temp_mode_running = false;
        return 0.0f;
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
            gpio_set_level(other_gpio, 0);
        }
    }

    float effective_percent = target_percent;
    if (control_type == PARAM_FAN_CONTROL_DC) {
        effective_percent = 100.0f - target_percent;
    } else if (control_type == PARAM_FAN_CONTROL_PWM && fan_control_inverted(control_type)) {
        effective_percent = 100.0f - target_percent;
    }
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

static fan_state_t fan_control_aggregate_state(void) {
    bool any_starting = false;
    bool any_running = false;

    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        if (g_state[ch] == FAN_STATE_IN_SERVICE) {
            return FAN_STATE_IN_SERVICE;
        }
        if (g_state[ch] == FAN_STATE_STALL) {
            return FAN_STATE_STALL;
        }
        if (g_state[ch] == FAN_STATE_STARTING) {
            any_starting = true;
        } else if (g_state[ch] == FAN_STATE_RUNNING) {
            any_running = true;
        }
    }

    if (any_starting) return FAN_STATE_STARTING;
    if (any_running) return FAN_STATE_RUNNING;
    return FAN_STATE_IDLE;
}

static void fan_control_set_state(uint8_t channel, fan_state_t next, int64_t now_us) {
    if (channel >= METRICS_FAN_CHANNELS) return;
    g_state[channel] = next;
    g_state_enter_us[channel] = now_us;
    if (next == FAN_STATE_RUNNING) {
        g_last_rpm_ok_us[channel] = now_us;
    }
    fan_status_cache_write(fan_control_current_operation());
}

static fan_state_t fan_control_get_state(uint8_t channel) {
    if (channel >= METRICS_FAN_CHANNELS) return FAN_STATE_IDLE;
    return g_state[channel];
}

static bool fan_control_monitoring_enabled(const params_t *params, uint8_t channel) {
    if (!params) return false;
    switch (channel) {
        case 0:
            return params->fan_monitoring_enabled != 0;
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

bool fan_control_override_set(uint8_t op_type, float target_rpm) {
    if (!g_override_active || g_override_op == (operation_type_t)op_type) {
        g_override_active = true;
        g_override_op = (operation_type_t)op_type;
        g_override_rpm = target_rpm;
        g_override_has_control_type = false;
        return true;
    }
    return false;
}

bool fan_control_override_set_output(uint8_t op_type, uint8_t control_type, float target_percent) {
    if (control_type != PARAM_FAN_CONTROL_DC && control_type != PARAM_FAN_CONTROL_PWM) {
        return false;
    }
    if (!g_override_active || g_override_op == (operation_type_t)op_type) {
        g_override_active = true;
        g_override_op = (operation_type_t)op_type;
        g_override_rpm = target_percent;
        g_override_has_control_type = true;
        g_override_control_type = control_type;
        return true;
    }
    return false;
}

void fan_control_override_clear(uint8_t op_type) {
    if (g_override_active && g_override_op == (operation_type_t)op_type) {
        g_override_active = false;
        g_override_op = OP_TYPE_NONE;
        g_override_rpm = 0.0f;
        g_override_has_control_type = false;
        g_override_control_type = PARAM_FAN_CONTROL_DC;
    }
}

void fan_control_cycle_limit_reset(void) {
    fan_control_cycle_reset();
}

bool fan_control_overcurrent_recovery_active(void) {
    return g_recovery_active;
}

float fan_control_cycle_max_percent(void) {
    if (g_recovery_active || g_cycle_limit_locked) {
        return g_cycle_max_percent;
    }
    return 100.0f;
}

bool fan_control_force_overcurrent_recovery(float applied_percent) {
    if (!(applied_percent > 0.0f)) {
        return false;
    }
    if (g_recovery_active) {
        return true;
    }
    fan_control_begin_overcurrent_recovery(applied_percent, esp_timer_get_time());
    return true;
}

static operation_type_t fan_control_current_operation(void) {
    if (!operation_manager_is_active()) {
        return OP_TYPE_NONE;
    }
    return operation_manager_get_active_type();
}

static bool fan_control_override_get(operation_type_t *op_type,
                                     float *rpm,
                                     bool *has_control_type,
                                     uint8_t *control_type) {
    bool active = g_override_active;
    if (active) {
        if (op_type) *op_type = g_override_op;
        if (rpm) *rpm = g_override_rpm;
        if (has_control_type) *has_control_type = g_override_has_control_type;
        if (control_type) *control_type = g_override_control_type;
    }
    return active;
}

static void fan_control_notify_state(void) {
    operation_type_t op = fan_control_current_operation();
    bool changed = (op != g_last_reported_op);
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        if (g_state[ch] != g_last_reported[ch]) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return;
    }
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        g_last_reported[ch] = g_state[ch];
    }
    g_last_reported_op = op;
    fan_status_cache_write(op);
    uint8_t payload[FAN_STATUS_PAYLOAD_LEN] = {
        FAN_STATUS_VERSION,
        (uint8_t)g_state[0],
        (uint8_t)g_state[1],
        (uint8_t)g_state[2],
        (uint8_t)g_state[3],
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
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        out[1 + ch] = (uint8_t)snap.state[ch];
    }
    out[1 + METRICS_FAN_CHANNELS] = (uint8_t)snap.op;
}

void fan_control_init(void) {
    int64_t now = esp_timer_get_time();
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        g_state[ch] = FAN_STATE_IDLE;
        g_state_enter_us[ch] = now;
        g_last_rpm_ok_us[ch] = 0;
        g_last_reported[ch] = FAN_STATE_IDLE;
    }
    g_last_reported_op = OP_TYPE_NONE;
    g_override_active = false;
    g_override_op = OP_TYPE_NONE;
    g_override_rpm = 0.0f;
    g_override_has_control_type = false;
    g_override_control_type = PARAM_FAN_CONTROL_DC;
    fan_control_cycle_reset();
    device_status_set_error_flag(DEVICE_ERROR_OVERHEAT, false);
    fan_status_cache_write(OP_TYPE_NONE);
}

void fan_control_task(void *param) {
    (void)param;
    const TickType_t delay = pdMS_TO_TICKS(200);

    while (1) {
        int64_t now = esp_timer_get_time();
        operation_manager_tick(now);

        fan_state_t aggregate_state = fan_control_aggregate_state();
        bool op_active = operation_manager_is_active();
        operation_type_t op_type = op_active ? operation_manager_get_active_type() : OP_TYPE_NONE;
        operation_type_t override_op = OP_TYPE_NONE;
        float override_rpm = 0.0f;
        bool override_has_control_type = false;
        uint8_t override_control_type = PARAM_FAN_CONTROL_DC;
        bool override_active = fan_control_override_get(&override_op,
                                                        &override_rpm,
                                                        &override_has_control_type,
                                                        &override_control_type);

        if (op_active) {
            device_status_set_error_flag(DEVICE_ERROR_OVERHEAT, false);
            device_status_set_error_flag(DEVICE_ERROR_NTC_DISCONNECTED, false);
            if (aggregate_state != FAN_STATE_IN_SERVICE) {
                for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
                    fan_control_set_state(ch, FAN_STATE_IN_SERVICE, now);
                }
                fan_control_notify_state();
            }
            params_t params;
            uint8_t control_type = PARAM_FAN_CONTROL_DC;
            if (params_cache_get(&params)) {
                control_type = params.fan_control_type;
            }
            if (override_active && override_op == op_type) {
                if (override_has_control_type) {
                    control_type = override_control_type;
                }
                fan_control_apply_output(control_type, fan_control_apply_cycle_limit(override_rpm, now));
            } else {
                fan_control_apply_output(control_type, fan_control_apply_cycle_limit(0.0f, now));
            }
            vTaskDelay(delay);
            continue;
        }

        if (aggregate_state == FAN_STATE_IN_SERVICE) {
            for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
                fan_control_set_state(ch, FAN_STATE_IDLE, now);
            }
            fan_control_notify_state();
            aggregate_state = FAN_STATE_IDLE;
        }

        params_t params;
        if (!params_cache_get(&params)) {
            device_status_set_error_flag(DEVICE_ERROR_OVERHEAT, false);
            device_status_set_error_flag(DEVICE_ERROR_NTC_DISCONNECTED, false);
            vTaskDelay(delay);
            continue;
        }

        float temp = metrics_get_temp(FAN_TEMP_CONTROL_CHANNEL);
        bool temp_sensor_disconnected =
            params.fan_mode == PARAM_FAN_MODE_TEMP_SENSOR &&
            !isfinite(temp);
        device_status_set_error_flag(DEVICE_ERROR_NTC_DISCONNECTED, temp_sensor_disconnected);
        bool overheat = params.fan_mode == PARAM_FAN_MODE_TEMP_SENSOR &&
                        isfinite(temp) &&
                        temp >= (float)params.fan_max_temp;
        device_status_set_error_flag(DEVICE_ERROR_OVERHEAT, overheat);
        float target_percent = fan_control_regulate(&params, temp);
        if (temp_sensor_disconnected) {
            target_percent = (float)params.fan_min_speed;
        }
        target_percent = fan_control_apply_cycle_limit(target_percent, now);
        bool command_on = target_percent > 0.0f;
        bool fan_required = command_on;

        fan_control_apply_output(params.fan_control_type, target_percent);

        if (!fan_required) {
            if (aggregate_state != FAN_STATE_IDLE) {
                for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
                    fan_control_set_state(ch, FAN_STATE_IDLE, now);
                }
                fan_control_notify_state();
            }
            vTaskDelay(delay);
            continue;
        }

        for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
            fan_state_t state = fan_control_get_state(ch);
            float fan_rpm = metrics_get_fan_speed_rpm_channel(ch);
            bool rpm_ok = isfinite(fan_rpm) && fan_rpm > 0.0f;

            if (!fan_control_monitoring_enabled(&params, ch)) {
                fan_state_t next = rpm_ok ? FAN_STATE_RUNNING : FAN_STATE_IDLE;
                if (state != next) {
                    fan_control_set_state(ch, next, now);
                }
                continue;
            }

            if (rpm_ok) {
                g_last_rpm_ok_us[ch] = now;
            }

            switch (state) {
                case FAN_STATE_IDLE:
                    fan_control_set_state(ch, FAN_STATE_STARTING, now);
                    break;
                case FAN_STATE_STARTING:
                    if (rpm_ok) {
                        fan_control_set_state(ch, FAN_STATE_RUNNING, now);
                    } else if (now - g_state_enter_us[ch] >= FAN_START_TIMEOUT_US) {
                        ESP_LOGE(FAN_TAG, "Fan %u stall detected (start timeout), rpm=%.1f",
                                 (unsigned)(ch + 1U), (double)fan_rpm);
                        fan_control_set_state(ch, FAN_STATE_STALL, now);
                    }
                    break;
                case FAN_STATE_RUNNING:
                    if (!rpm_ok && now - g_last_rpm_ok_us[ch] >= FAN_FAIL_TIMEOUT_US) {
                        ESP_LOGE(FAN_TAG, "Fan %u stall detected (run timeout), rpm=%.1f",
                                 (unsigned)(ch + 1U), (double)fan_rpm);
                        fan_control_set_state(ch, FAN_STATE_STALL, now);
                    }
                    break;
                case FAN_STATE_STALL:
                    if (rpm_ok) {
                        fan_control_set_state(ch, FAN_STATE_RUNNING, now);
                    }
                    break;
                default:
                    fan_control_set_state(ch, FAN_STATE_IDLE, now);
                    break;
            }
        }
        fan_control_notify_state();

        vTaskDelay(delay);
    }
}
