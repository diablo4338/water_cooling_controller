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
#include "output_limit.h"
#include "thermal_protection.h"
#include "params.h"
#include "state.h"

#define FAN_TAG "fan-ctl"

#define FAN_CHANNEL_SCAN_TIMEOUT_US (METRICS_FAN_CHANNELS * 1000000LL)
#define FAN_START_TIMEOUT_US (METRICS_TACH_SCAN_US + FAN_CHANNEL_SCAN_TIMEOUT_US)
#define FAN_FAIL_TIMEOUT_US FAN_CHANNEL_SCAN_TIMEOUT_US
#define FAN_HIGHSIDE_GPIO 3
#define FAN_PWM_GPIO_DC 10
#define FAN_PWM_GPIO_PWM 2
#define FAN_PWM_FREQ_HZ 25000
#define FAN_DC_FREQ_HZ 1000
#define FAN_PWM_RES_BITS LEDC_TIMER_10_BIT
#define FAN_DC_RES_BITS LEDC_TIMER_10_BIT
static const ledc_mode_t FAN_PWM_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_channel_t FAN_PWM_CHANNEL_ACTIVE = LEDC_CHANNEL_0;
static const ledc_timer_t FAN_PWM_TIMER = LEDC_TIMER_0;

static uint32_t fan_control_max_duty(ledc_timer_bit_t res_bits) {
    return (1U << (uint32_t)res_bits) - 1U;
}
typedef struct {
    fan_state_t state;
    int64_t entered_us;
    int64_t last_rpm_ok_us;
} fan_channel_t;

static fan_channel_t g_channels[METRICS_FAN_CHANNELS];
static bool g_override_active = false;
static operation_type_t g_override_op = OP_TYPE_NONE;
static float g_override_rpm = 0.0f;
static uint8_t g_override_control_type = PARAM_FAN_CONTROL_DC;
static float g_output_percent = NAN;
static uint8_t g_applied_control_type = UINT8_MAX;
typedef struct {
    fan_state_t state[METRICS_FAN_CHANNELS];
    operation_type_t op;
} fan_status_t;

static fan_status_t g_status;
static uint8_t g_mode_status[FAN_MODE_STATUS_PAYLOAD_LEN] = {
    FAN_MODE_STATUS_VERSION, PARAM_FAN_MODE_CONTINUOUS, PARAM_FAN_MODE_CONTINUOUS,
    THERMAL_INPUT_ADC_INITIALIZING,
};

void fan_control_get_mode_payload(uint8_t *out, size_t len) {
    if (!out || len < sizeof(g_mode_status)) return;
    state_lock();
    for (size_t i = 0; i < sizeof(g_mode_status); i++) out[i] = g_mode_status[i];
    state_unlock();
}


static bool fan_control_inverted(uint8_t control_type) {
    return control_type == PARAM_FAN_CONTROL_PWM;
}

typedef enum {
    TEMP_DEMAND_IDLE = 0,
    TEMP_DEMAND_RUNNING,
} temp_demand_state_t;

typedef enum {
    TEMP_DEMAND_CONTROL_DISABLED,
    TEMP_DEMAND_START_REACHED,
    TEMP_DEMAND_STOP_REACHED,
} temp_demand_event_t;

static temp_demand_state_t g_temp_demand = TEMP_DEMAND_IDLE;

static void temp_demand_dispatch(temp_demand_event_t event) {
    switch (event) {
        case TEMP_DEMAND_CONTROL_DISABLED:
        case TEMP_DEMAND_STOP_REACHED:
            g_temp_demand = TEMP_DEMAND_IDLE;
            break;
        case TEMP_DEMAND_START_REACHED:
            g_temp_demand = TEMP_DEMAND_RUNNING;
            break;
    }
}

static float fan_control_regulate(const params_t *params, uint8_t effective_mode, float temp_c) {
    if (params->fan_mode != PARAM_FAN_MODE_TEMP_SENSOR) {
        temp_demand_dispatch(TEMP_DEMAND_CONTROL_DISABLED);
    }
    // Temporary fallback keeps the temperature demand for when the sensor recovers.
    if (effective_mode == PARAM_FAN_MODE_CONTINUOUS) return (float)params->fan_min_speed;
    if (effective_mode != PARAM_FAN_MODE_TEMP_SENSOR) return 0.0f;

    float start_temp = (float)params->fan_start_temp;
    float off_temp = start_temp - (float)params->fan_off_delta;
    float max_temp = (float)params->fan_max_temp;
    if (temp_c >= start_temp) {
        temp_demand_dispatch(TEMP_DEMAND_START_REACHED);
    } else if (temp_c <= off_temp) {
        temp_demand_dispatch(TEMP_DEMAND_STOP_REACHED);
    }
    if (g_temp_demand == TEMP_DEMAND_IDLE) return 0.0f;

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

static void fan_control_publish_applied_output(float percent) {
    state_lock();
    g_output_percent = percent;
    state_unlock();
}

static esp_err_t fan_control_disable_power(void) {
    static bool highside_ready;
    esp_err_t err = gpio_set_level(FAN_HIGHSIDE_GPIO, 0);
    if (err == ESP_OK && !highside_ready) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << FAN_HIGHSIDE_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&io);
        highside_ready = err == ESP_OK;
    }
    fan_control_publish_applied_output(err == ESP_OK ? 0.0f : NAN);
    return err;
}

static esp_err_t fan_control_configure_pwm(uint8_t control_type, uint32_t duty) {
    bool pwm = control_type == PARAM_FAN_CONTROL_PWM;
    int gpio = pwm ? FAN_PWM_GPIO_PWM : FAN_PWM_GPIO_DC;
    int other_gpio = pwm ? FAN_PWM_GPIO_DC : FAN_PWM_GPIO_PWM;
    const ledc_timer_config_t timer = {
        .speed_mode = FAN_PWM_MODE,
        .duty_resolution = pwm ? FAN_PWM_RES_BITS : FAN_DC_RES_BITS,
        .timer_num = FAN_PWM_TIMER,
        .freq_hz = pwm ? FAN_PWM_FREQ_HZ : FAN_DC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;

    const gpio_config_t other_io = {
        .pin_bit_mask = 1ULL << other_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&other_io);
    if (err != ESP_OK) return err;
    err = gpio_set_level(other_gpio, 0);
    if (err != ESP_OK) return err;

    const ledc_channel_config_t channel = {
        .gpio_num = gpio,
        .speed_mode = FAN_PWM_MODE,
        .channel = FAN_PWM_CHANNEL_ACTIVE,
        .timer_sel = FAN_PWM_TIMER,
        .duty = duty,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

static bool fan_control_apply_output(uint8_t control_type, float target_percent) {
    static uint8_t configured_type = UINT8_MAX;
    static uint32_t applied_duty;
    float target = fmaxf(0.0f, fminf(target_percent, 100.0f));
    esp_err_t err;

    if (target == 0.0f) {
        err = g_output_percent == 0.0f ? ESP_OK : fan_control_disable_power();
    } else {
        bool pwm = control_type == PARAM_FAN_CONTROL_PWM;
        float effective = pwm && !fan_control_inverted(control_type) ? target : 100.0f - target;
        uint32_t range = fan_control_max_duty(pwm ? FAN_PWM_RES_BITS : FAN_DC_RES_BITS);
        uint32_t duty = (uint32_t)lroundf(effective * (float)range / 100.0f);
        bool configure = configured_type != control_type;
        bool enable_power = configure || !isfinite(g_output_percent) || g_output_percent == 0.0f;
        err = enable_power ? fan_control_disable_power() : ESP_OK;
        if (err == ESP_OK && configure) {
            err = fan_control_configure_pwm(control_type, duty);
        }
        if (err == ESP_OK && (configure || duty != applied_duty)) {
            err = ledc_set_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL_ACTIVE, duty);
            if (err == ESP_OK) err = ledc_update_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL_ACTIVE);
        }
        if (err == ESP_OK && enable_power) err = gpio_set_level(FAN_HIGHSIDE_GPIO, 1);
        if (err == ESP_OK) {
            configured_type = control_type;
            applied_duty = duty;
            g_applied_control_type = control_type;
            fan_control_publish_applied_output(target);
        }
    }

    if (err != ESP_OK) {
        configured_type = UINT8_MAX;
        esp_err_t off_err = fan_control_disable_power();
        ESP_LOGE(FAN_TAG, "Fan output failed: %s; power-off result: %s",
                 esp_err_to_name(err), esp_err_to_name(off_err));
    }
    device_status_set_error_flag(DEVICE_ERROR_FAN_OUTPUT, err != ESP_OK);
    return err == ESP_OK;
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

bool fan_control_override_set_output(uint8_t op_type, uint8_t control_type, float target_percent) {
    if (control_type != PARAM_FAN_CONTROL_DC && control_type != PARAM_FAN_CONTROL_PWM) {
        return false;
    }
    if (!g_override_active || g_override_op == (operation_type_t)op_type) {
        g_override_active = true;
        g_override_op = (operation_type_t)op_type;
        g_override_rpm = target_percent;
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
        g_override_control_type = PARAM_FAN_CONTROL_DC;
    }
}

float fan_control_get_output_percent(void) {
    state_lock();
    float percent = g_output_percent;
    state_unlock();
    return percent;
}

static bool fan_control_override_get(operation_type_t *op_type,
                                     float *rpm,
                                     uint8_t *control_type) {
    bool active = g_override_active;
    if (active) {
        if (op_type) *op_type = g_override_op;
        if (rpm) *rpm = g_override_rpm;
        if (control_type) *control_type = g_override_control_type;
    }
    return active;
}

static void fan_status_encode(const fan_status_t *status, uint8_t *out) {
    out[0] = FAN_STATUS_VERSION;
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        out[1 + ch] = (uint8_t)status->state[ch];
    }
    out[1 + METRICS_FAN_CHANNELS] = (uint8_t)status->op;
}

static void fan_control_publish_status(operation_type_t op) {
    fan_status_t status = {.op = op};
    bool changed = g_status.op != op;
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        status.state[ch] = g_channels[ch].state;
        changed |= status.state[ch] != g_status.state[ch];
    }
    if (!changed) return;

    state_lock();
    g_status = status;
    state_unlock();

    uint8_t payload[FAN_STATUS_PAYLOAD_LEN];
    fan_status_encode(&status, payload);
    fan_status_notify(fsm_get_auth_conn_handle(), payload, sizeof(payload));
}

void fan_control_get_status_payload(uint8_t *out, size_t len) {
    if (!out || len < FAN_STATUS_PAYLOAD_LEN) return;
    state_lock();
    fan_status_t status = g_status;
    state_unlock();
    fan_status_encode(&status, out);
}

static void fan_channel_update(uint8_t ch, operation_type_t op, const params_t *params,
                               bool command_on, int64_t now_us) {
    fan_channel_t *channel = &g_channels[ch];
    fan_state_t next = channel->state;
    float rpm = 0.0f;

    if (op != OP_TYPE_NONE) {
        next = FAN_STATE_IN_SERVICE;
    } else if (params == NULL) {
        if (next == FAN_STATE_IN_SERVICE) next = FAN_STATE_IDLE;
    } else if (!command_on) {
        next = FAN_STATE_IDLE;
    } else {
        rpm = metrics_get_fan_speed_rpm_channel(ch);
        bool rpm_ok = isfinite(rpm) && rpm > 0.0f;
        if (rpm_ok) channel->last_rpm_ok_us = now_us;

        if (!fan_control_monitoring_enabled(params, ch)) {
            next = rpm_ok ? FAN_STATE_RUNNING : FAN_STATE_IDLE;
        } else {
            switch (channel->state) {
                case FAN_STATE_IN_SERVICE:
                case FAN_STATE_IDLE:
                    next = FAN_STATE_STARTING;
                    break;
                case FAN_STATE_STARTING:
                    if (rpm_ok) {
                        next = FAN_STATE_RUNNING;
                    } else if (now_us - channel->entered_us >= FAN_START_TIMEOUT_US) {
                        next = FAN_STATE_STALL;
                    }
                    break;
                case FAN_STATE_RUNNING:
                    if (!rpm_ok && now_us - channel->last_rpm_ok_us >= FAN_FAIL_TIMEOUT_US) {
                        next = FAN_STATE_STALL;
                    }
                    break;
                case FAN_STATE_STALL:
                    if (rpm_ok) next = FAN_STATE_RUNNING;
                    break;
            }
        }
    }

    if (next != channel->state) {
        if (next == FAN_STATE_STALL) {
            const char *reason = channel->state == FAN_STATE_STARTING ? "start" : "run";
            ESP_LOGE(FAN_TAG, "Fan %u stall detected (%s timeout), rpm=%.1f",
                     (unsigned)(ch + 1U), reason, (double)rpm);
        }
        channel->state = next;
        channel->entered_us = now_us;
    }
}

void fan_control_init(void) {
    int64_t now = esp_timer_get_time();
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        g_channels[ch] = (fan_channel_t){.state = FAN_STATE_IDLE, .entered_us = now};
    }
    g_status = (fan_status_t){.op = OP_TYPE_NONE};
    g_override_active = false;
    g_override_op = OP_TYPE_NONE;
    g_override_rpm = 0.0f;
    g_override_control_type = PARAM_FAN_CONTROL_DC;
    temp_demand_dispatch(TEMP_DEMAND_CONTROL_DISABLED);
    fan_control_apply_output(PARAM_FAN_CONTROL_DC, 0.0f);
}

static float fan_control_update_output(operation_type_t op, const params_config_t *config, uint8_t effective_mode,
                                       float temp, int64_t now) {
    if (!isfinite(g_output_percent) && !fan_control_apply_output(PARAM_FAN_CONTROL_DC, 0.0f)) {
        return 0.0f;
    }
    const params_t *params = &config->params;
    uint8_t control_type = params->fan_control_type;
    float percent = 0.0f;
    if (op != OP_TYPE_NONE) {
        operation_type_t owner = OP_TYPE_NONE;
        if (!fan_control_override_get(&owner, &percent, &control_type) || owner != op) {
            percent = 0.0f;
            control_type = params->fan_control_type;
        }
    } else {
        percent = fan_control_regulate(params, effective_mode, temp);
    }

    output_limit_mode_t limit_mode = OUTPUT_LIMIT_MODE_CONTINUOUS;
    if (op != OP_TYPE_NONE) limit_mode = OUTPUT_LIMIT_MODE_SERVICE;
    else if (effective_mode == PARAM_FAN_MODE_TEMP_SENSOR) limit_mode = OUTPUT_LIMIT_MODE_TEMPERATURE;
    output_limit_command_t command = output_limit_step(percent, g_output_percent, control_type,
                                                      config->maximum, params->fan_min_speed, limit_mode, now);
    bool applied = fan_control_apply_output(command.control_type, command.percent);
    if (applied) output_limit_output_applied(g_output_percent, g_applied_control_type, esp_timer_get_time());
    return applied && op == OP_TYPE_NONE ? g_output_percent : 0.0f;
}

void fan_control_task(void *param) {
    (void)param;
    const TickType_t delay = pdMS_TO_TICKS(200);
    uint8_t previous_mode = PARAM_FAN_MODE_CONTINUOUS;

    while (1) {
        int64_t now = esp_timer_get_time();
        params_config_t config = params_read_config();
        const params_t *params = &config.params;
        uint8_t selected = params->fan_mode;
        bool temperature_control = selected == PARAM_FAN_MODE_TEMP_SENSOR;
        thermal_observation_t thermal = thermal_protection_update(temperature_control, params->fan_max_temp);
        uint8_t effective = temperature_control && thermal.input != THERMAL_INPUT_READY
                                ? PARAM_FAN_MODE_CONTINUOUS : selected;
        if (selected == PARAM_FAN_MODE_INACTIVE && previous_mode != PARAM_FAN_MODE_INACTIVE) {
            output_limit_request_stop();
        }
        previous_mode = selected;
        operation_manager_tick(now);

        operation_type_t op = operation_manager_get_active_type();
        bool command_on = fan_control_update_output(op, &config, effective, thermal.temperature, now) > 0.0f;
        operation_manager_check_output(now, g_output_percent, g_applied_control_type);

        state_lock();
        g_mode_status[1] = selected;
        g_mode_status[2] = op != OP_TYPE_NONE ? FAN_MODE_EFFECTIVE_SERVICE : effective;
        g_mode_status[3] = thermal.input;
        state_unlock();
        for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
            fan_channel_update(ch, op, params, command_on, now);
        }
        fan_control_publish_status(op);
        fan_runtime_status_publish();
        vTaskDelay(delay);
    }
}
