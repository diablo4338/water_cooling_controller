#include "metrics.h"

#include <math.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "ads1115.h"
#include "ina226.h"

static const char *METRICS_TAG = "metrics";

static float g_temp_values[METRICS_TEMP_CHANNELS] = {NAN, NAN, NAN, NAN};
static bool g_temp_valid[METRICS_TEMP_CHANNELS] = {false};
static uint8_t g_temp_failures[METRICS_TEMP_CHANNELS] = {0};
static bool g_ads_error = false;
static float g_voltage_v = NAN;
static float g_current_ma = NAN;
static bool g_power_valid = false;

volatile uint32_t dbg_fall = 0;
volatile uint32_t dbg_rise = 0;
volatile uint32_t dbg_low_reject = 0;
volatile uint32_t dbg_period_reject = 0;
volatile uint32_t dbg_period_ok = 0;
volatile uint32_t dbg_low_min_us = 0;
volatile uint32_t dbg_low_max_us = 0;
volatile uint32_t dbg_period_min_us = 0;
volatile uint32_t dbg_period_max_us = 0;

#define FAN_TACH_GPIO 18
#define FAN_TACH_SEL_GPIO0 6
#define FAN_TACH_SEL_GPIO1 7
#define FAN_TACH_PULSES_PER_REV 2
#define FAN_TACH_MIN_VALID_DT_US 10000U
#define FAN_TACH_MAX_VALID_DT_US 150000U
#define FAN_TACH_DT_BUF_SIZE 16
#define FAN_TACH_MIN_CALC_SAMPLES 2
#define FAN_TACH_TRIM_SAMPLES 0
#define FAN_TACH_STOP_TIMEOUT_US (METRICS_FAN_CHANNELS * 1000000LL)
#define FAN_TACH_SWITCH_DELAY_US 1000u
#define FAN_TACH_MIN_LOW_WIDTH_US 50u
#define METRICS_ADC_TASK_PERIOD_MS 250
#define METRICS_INA_TASK_PERIOD_MS 250
#define METRICS_TACH_CHANNEL_SETTLE_MS 400
#define METRICS_TASK_STACK_SIZE 4096
#define METRICS_TASK_PRIORITY 5
#define FAN_RPM_NOTIFY_EPS 1.0f
#define FAN_RPM_EMA_OLD 0.2f
#define FAN_RPM_EMA_NEW 0.8f
#define TEMP_NOTIFY_EPS 0.01f

typedef struct {
    volatile uint32_t dt_buf[FAN_TACH_DT_BUF_SIZE];
    volatile uint32_t dt_count;
    volatile uint32_t dt_idx;
    volatile int64_t last_falling_us;
    volatile int64_t last_valid_edge_us;
    volatile int64_t low_start_us;
    volatile uint8_t skip_edges;
    float rpm;
    bool valid;
    float filtered_rpm;
    bool filter_valid;
} fan_tach_channel_t;

static portMUX_TYPE g_fan_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
static fan_tach_channel_t g_fan_channels[METRICS_FAN_CHANNELS];
static volatile uint8_t g_fan_active_channel = 0;
static volatile int64_t g_fan_ignore_edges_until_us = 0;
static volatile uint16_t g_metrics_changed_mask = 0;
static uint32_t g_snapshot_seq = 0;
static metrics_snapshot_t g_snapshot = {0};

static const uint8_t g_ads_input_by_temp_channel[METRICS_TEMP_CHANNELS] = {1, 0, 3, 2};
static const uint8_t g_tach_select_by_fan_channel[METRICS_FAN_CHANNELS] = {0, 2, 3, 1};

static void fan_pcnt_init(void);
static void fan_tach_select_channel(uint8_t channel);
static void fan_tach_isr(void *arg);
static void metrics_snapshot_write_from_state(void);
static bool metrics_snapshot_read(metrics_snapshot_t *out);
static uint16_t metrics_update_power_sample(void);
static float metrics_round_temp_for_buffer(float temp_c);
static void metrics_publish_changes(uint16_t changed_mask);
static void metrics_adc_task(void *arg);
static void metrics_ina_task(void *arg);
static void metrics_tach_task(void *arg);

#define METRICS_FAIL_THRESHOLD 3
#define METRICS_RAW_NO_SENSOR_THRESHOLD 500

void metrics_init(void) {
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        g_temp_values[i] = NAN;
        g_temp_valid[i] = false;
        g_temp_failures[i] = 0;
    }
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
        g_fan_channels[ch].rpm = NAN;
        g_fan_channels[ch].valid = false;
        g_fan_channels[ch].filtered_rpm = 0.0f;
        g_fan_channels[ch].filter_valid = false;
        g_fan_channels[ch].last_falling_us = 0;
        g_fan_channels[ch].last_valid_edge_us = 0;
        g_fan_channels[ch].low_start_us = 0;
        g_fan_channels[ch].skip_edges = 0;
        g_fan_channels[ch].dt_count = 0;
        g_fan_channels[ch].dt_idx = 0;
        for (int i = 0; i < FAN_TACH_DT_BUF_SIZE; i++) {
            g_fan_channels[ch].dt_buf[i] = 0;
        }
    }
    g_fan_active_channel = 0;
    g_fan_ignore_edges_until_us = 0;
    g_voltage_v = NAN;
    g_current_ma = NAN;
    g_power_valid = false;
    bool ads_ok = ads1115_init();
    bool ina_ok = ina226_init();
    g_ads_error = !ads_ok;
    (void)ina_ok;
    fan_pcnt_init();
    metrics_snapshot_write_from_state();
    xTaskCreate(metrics_adc_task, "metrics_adc", METRICS_TASK_STACK_SIZE, NULL, METRICS_TASK_PRIORITY, NULL);
    xTaskCreate(metrics_ina_task, "metrics_ina", METRICS_TASK_STACK_SIZE, NULL, METRICS_TASK_PRIORITY, NULL);
    xTaskCreate(metrics_tach_task, "metrics_tach", METRICS_TASK_STACK_SIZE, NULL, METRICS_TASK_PRIORITY, NULL);
}

float metrics_get_temp(uint8_t channel) {
    if (channel >= METRICS_TEMP_CHANNELS) return NAN;
    metrics_snapshot_t snap;
    metrics_get_snapshot(&snap);
    if (!snap.temp_valid[channel]) return NAN;
    return snap.temp_c[channel];
}

float metrics_get_fan_speed_rpm_channel(uint8_t channel) {
    if (channel >= METRICS_FAN_CHANNELS) return NAN;
    metrics_snapshot_t snap;
    metrics_get_snapshot(&snap);
    if (!snap.fan_valid[channel]) return NAN;
    return snap.fan_rpm[channel];
}

float metrics_get_voltage_v(void) {
    metrics_snapshot_t snap;
    metrics_get_snapshot(&snap);
    if (!snap.power_valid) return NAN;
    return snap.voltage_v;
}

float metrics_get_current_ma(void) {
    metrics_snapshot_t snap;
    metrics_get_snapshot(&snap);
    if (!snap.power_valid) return NAN;
    return snap.current_ma;
}

static void fan_pcnt_init(void) {
    gpio_config_t sel_io = {
        .pin_bit_mask = (1ULL << FAN_TACH_SEL_GPIO0) | (1ULL << FAN_TACH_SEL_GPIO1),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sel_io));
    fan_tach_select_channel(0);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FAN_TACH_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(METRICS_TAG, "gpio_install_isr_service failed: %d", err);
    }
    err = gpio_isr_handler_add(FAN_TACH_GPIO, fan_tach_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(METRICS_TAG, "gpio_isr_handler_add failed: %d", err);
    }
}

static void fan_tach_select_channel(uint8_t channel) {
    if (channel >= METRICS_FAN_CHANNELS) return;

    uint8_t select = g_tach_select_by_fan_channel[channel];
    gpio_set_level(FAN_TACH_SEL_GPIO0, select & 0x01U);
    gpio_set_level(FAN_TACH_SEL_GPIO1, (select >> 1) & 0x01U);

    int64_t ignore_until = esp_timer_get_time() + FAN_TACH_SWITCH_DELAY_US;
    portENTER_CRITICAL(&g_fan_mux);
    g_fan_active_channel = channel;
    g_fan_ignore_edges_until_us = ignore_until;
    g_fan_channels[channel].last_falling_us = 0;
    g_fan_channels[channel].low_start_us = 0;
    g_fan_channels[channel].skip_edges = 2;
    portEXIT_CRITICAL(&g_fan_mux);
}

static bool metrics_update_value(uint8_t channel, float temp_c) {
    if (channel >= METRICS_TEMP_CHANNELS) return false;
    if (!isfinite(temp_c)) return false;

    temp_c = metrics_round_temp_for_buffer(temp_c);

    bool changed = false;
    portENTER_CRITICAL(&g_metrics_mux);
    if (!g_temp_valid[channel] || fabsf(g_temp_values[channel] - temp_c) >= TEMP_NOTIFY_EPS) {
        g_temp_values[channel] = temp_c;
        g_temp_valid[channel] = true;
        changed = true;
    }
    portEXIT_CRITICAL(&g_metrics_mux);
    return changed;
}

static float metrics_round_temp_for_buffer(float temp_c) {
    return roundf(temp_c * 10.0f) / 10.0f;
}

static bool metrics_mark_invalid(uint8_t channel) {
    if (channel >= METRICS_TEMP_CHANNELS) return false;
    portENTER_CRITICAL(&g_metrics_mux);
    if (!g_temp_valid[channel]) {
        portEXIT_CRITICAL(&g_metrics_mux);
        return false;
    }
    g_temp_values[channel] = NAN;
    g_temp_valid[channel] = false;
    portEXIT_CRITICAL(&g_metrics_mux);
    return true;
}

static bool metrics_calc_fan_rpm(uint8_t channel, float *out_rpm) {
    if (channel >= METRICS_FAN_CHANNELS) return false;
    int64_t now = esp_timer_get_time();
    uint32_t count = 0;
    int64_t last_valid_edge = 0;
    uint32_t buf[FAN_TACH_DT_BUF_SIZE] = {0};
    bool stopped = false;

    portENTER_CRITICAL(&g_fan_mux);
    fan_tach_channel_t *fan = &g_fan_channels[channel];
    last_valid_edge = fan->last_valid_edge_us;
    if (last_valid_edge != 0 && (now - last_valid_edge) > FAN_TACH_STOP_TIMEOUT_US) {
        stopped = true;
        fan->dt_count = 0;
        fan->dt_idx = 0;
        fan->last_falling_us = 0;
        fan->low_start_us = 0;
        for (uint32_t i = 0; i < FAN_TACH_DT_BUF_SIZE; i++) {
            fan->dt_buf[i] = 0;
        }
    } else {
        count = fan->dt_count;
        for (uint32_t i = 0; i < FAN_TACH_DT_BUF_SIZE; i++) {
            buf[i] = fan->dt_buf[i];
        }
    }
    portEXIT_CRITICAL(&g_fan_mux);

    if (last_valid_edge == 0) {
        return false;
    }
    if (stopped) {
        g_fan_channels[channel].filtered_rpm = 0.0f;
        g_fan_channels[channel].filter_valid = false;
        *out_rpm = 0.0f;
        return true;
    }
    if (count < FAN_TACH_MIN_CALC_SAMPLES) {
        return false;
    }
    if (count > FAN_TACH_DT_BUF_SIZE) {
        count = FAN_TACH_DT_BUF_SIZE;
    }
    uint32_t values[FAN_TACH_DT_BUF_SIZE];
    uint32_t used = 0;
    for (uint32_t i = 0; i < FAN_TACH_DT_BUF_SIZE && used < count; i++) {
        if (buf[i] > 0) {
            values[used++] = buf[i];
        }
    }
    if (used < FAN_TACH_MIN_CALC_SAMPLES) {
        return false;
    }
    for (uint32_t i = 1; i < used; i++) {
        uint32_t key = values[i];
        int j = (int)i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }

    uint32_t stable_count = used - (FAN_TACH_TRIM_SAMPLES * 2U);
    uint32_t stable_start = FAN_TACH_TRIM_SAMPLES;
    float stable_dt_us = 0.0f;
    if (stable_count & 1U) {
        stable_dt_us = (float)values[stable_start + stable_count / 2U];
    } else {
        uint32_t a = values[stable_start + (stable_count / 2U) - 1U];
        uint32_t b = values[stable_start + stable_count / 2U];
        stable_dt_us = 0.5f * ((float)a + (float)b);
    }
    if (!(stable_dt_us > 0.0f)) {
        return false;
    }

    float raw_rpm = (60.0f * 1000000.0f) / (stable_dt_us * (float)FAN_TACH_PULSES_PER_REV);
    if (!isfinite(raw_rpm) || raw_rpm < 0.0f) {
        return false;
    }
    if (!g_fan_channels[channel].filter_valid) {
        g_fan_channels[channel].filtered_rpm = raw_rpm;
        g_fan_channels[channel].filter_valid = true;
    } else {
        g_fan_channels[channel].filtered_rpm =
            g_fan_channels[channel].filtered_rpm * FAN_RPM_EMA_OLD + raw_rpm * FAN_RPM_EMA_NEW;
    }
    *out_rpm = g_fan_channels[channel].filtered_rpm;
    return true;
}

static void IRAM_ATTR fan_tach_isr(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(FAN_TACH_GPIO);
    portENTER_CRITICAL_ISR(&g_fan_mux);
    if (now < g_fan_ignore_edges_until_us) {
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }
    uint8_t channel = g_fan_active_channel;
    if (channel >= METRICS_FAN_CHANNELS) {
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }

    fan_tach_channel_t *fan = &g_fan_channels[channel];
    if (fan->skip_edges > 0) {
        if (level == 0) {
            dbg_fall++;
            fan->skip_edges--;
        } else {
            dbg_rise++;
        }
        fan->last_falling_us = 0;
        fan->low_start_us = 0;
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }

    if (level == 0) {
        dbg_fall++;
        fan->low_start_us = now;
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }

    dbg_rise++;
    int64_t low_start_us = fan->low_start_us;
    fan->low_start_us = 0;
    if (low_start_us == 0) {
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }
    int64_t low_width64 = now - low_start_us;
    if (low_width64 < FAN_TACH_MIN_LOW_WIDTH_US) {
        dbg_low_reject++;
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }
    uint32_t low_width_us = (uint32_t)low_width64;
    if (dbg_low_min_us == 0 || low_width_us < dbg_low_min_us) {
        dbg_low_min_us = low_width_us;
    }
    if (low_width_us > dbg_low_max_us) {
        dbg_low_max_us = low_width_us;
    }

    int64_t last_falling_us = fan->last_falling_us;
    fan->last_falling_us = low_start_us;
    if (last_falling_us == 0) {
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }
    int64_t period64 = low_start_us - last_falling_us;
    if (period64 < FAN_TACH_MIN_VALID_DT_US || period64 > FAN_TACH_MAX_VALID_DT_US) {
        dbg_period_reject++;
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }

    uint32_t dt_us = (uint32_t)period64;
    dbg_period_ok++;
    if (dbg_period_min_us == 0 || dt_us < dbg_period_min_us) {
        dbg_period_min_us = dt_us;
    }
    if (dt_us > dbg_period_max_us) {
        dbg_period_max_us = dt_us;
    }
    fan->last_valid_edge_us = now;
    uint32_t idx = fan->dt_idx;
    fan->dt_buf[idx] = dt_us;
    fan->dt_idx = (idx + 1) % FAN_TACH_DT_BUF_SIZE;
    if (fan->dt_count < FAN_TACH_DT_BUF_SIZE) {
        fan->dt_count++;
    }
    portEXIT_CRITICAL_ISR(&g_fan_mux);
}

static bool metrics_update_fan(uint8_t channel, float rpm) {
    if (channel >= METRICS_FAN_CHANNELS) return false;
    fan_tach_channel_t *fan = &g_fan_channels[channel];
    bool changed = false;
    portENTER_CRITICAL(&g_metrics_mux);
    if (!isfinite(rpm) || rpm < 0.0f) {
        if (!fan->valid) {
            portEXIT_CRITICAL(&g_metrics_mux);
            return false;
        }
        fan->valid = false;
        portEXIT_CRITICAL(&g_metrics_mux);
        return true;
    }
    changed = !fan->valid || fabsf(fan->rpm - rpm) >= FAN_RPM_NOTIFY_EPS;
    fan->valid = true;
    fan->rpm = rpm;
    portEXIT_CRITICAL(&g_metrics_mux);
    return changed;
}

static bool metrics_mark_fan_invalid(uint8_t channel) {
    if (channel >= METRICS_FAN_CHANNELS) return false;
    fan_tach_channel_t *fan = &g_fan_channels[channel];
    portENTER_CRITICAL(&g_metrics_mux);
    if (!fan->valid) {
        portEXIT_CRITICAL(&g_metrics_mux);
        return false;
    }
    fan->valid = false;
    fan->filter_valid = false;
    fan->filtered_rpm = 0.0f;
    portEXIT_CRITICAL(&g_metrics_mux);
    return true;
}

void metrics_get_snapshot(metrics_snapshot_t *out) {
    if (!out) return;
    metrics_snapshot_read(out);
}

uint16_t metrics_sample_all(void) {
    uint16_t changed_mask = 0;
    portENTER_CRITICAL(&g_metrics_mux);
    changed_mask = g_metrics_changed_mask;
    g_metrics_changed_mask = 0;
    portEXIT_CRITICAL(&g_metrics_mux);
    return changed_mask;
}

bool metrics_has_error(void) {
    return metrics_has_ads_error() || metrics_has_ina_error();
}

bool metrics_has_ads_error(void) {
    return g_ads_error;
}

bool metrics_has_ina_error(void) {
    return ina226_has_error();
}

static void metrics_snapshot_write_from_state(void) {
    metrics_snapshot_t snap;
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        snap.temp_c[i] = g_temp_values[i];
        snap.temp_valid[i] = g_temp_valid[i] ? 1U : 0U;
    }
    for (int i = 0; i < METRICS_FAN_CHANNELS; i++) {
        snap.fan_rpm[i] = g_fan_channels[i].rpm;
        snap.fan_valid[i] = g_fan_channels[i].valid ? 1U : 0U;
    }
    snap.voltage_v = g_voltage_v;
    snap.current_ma = g_current_ma;
    snap.power_valid = g_power_valid ? 1U : 0U;

    __atomic_fetch_add(&g_snapshot_seq, 1U, __ATOMIC_ACQ_REL);
    g_snapshot = snap;
    __atomic_fetch_add(&g_snapshot_seq, 1U, __ATOMIC_ACQ_REL);
}

static uint16_t metrics_update_power_sample(void) {
    ina226_sample_t sample;
    if (!ina226_get_sample(&sample)) {
        portENTER_CRITICAL(&g_metrics_mux);
        if (!g_power_valid) {
            portEXIT_CRITICAL(&g_metrics_mux);
            return 0;
        }
        g_power_valid = false;
        g_voltage_v = NAN;
        g_current_ma = NAN;
        portEXIT_CRITICAL(&g_metrics_mux);
        return METRICS_VOLTAGE_CHANGED_BIT | METRICS_CURRENT_CHANGED_BIT;
    }

    uint16_t changed = 0;
    portENTER_CRITICAL(&g_metrics_mux);
    if (!g_power_valid || fabsf(g_voltage_v - sample.voltage_v) >= 0.01f) {
        g_voltage_v = sample.voltage_v;
        changed |= METRICS_VOLTAGE_CHANGED_BIT;
    }
    if (!g_power_valid || fabsf(g_current_ma - sample.current_ma) >= 0.1f) {
        g_current_ma = sample.current_ma;
        changed |= METRICS_CURRENT_CHANGED_BIT;
    }
    g_power_valid = true;
    portEXIT_CRITICAL(&g_metrics_mux);
    return changed;
}

static void metrics_publish_changes(uint16_t changed_mask) {
    if (changed_mask == 0) {
        return;
    }
    portENTER_CRITICAL(&g_metrics_mux);
    g_metrics_changed_mask |= changed_mask;
    metrics_snapshot_write_from_state();
    portEXIT_CRITICAL(&g_metrics_mux);
}

static void metrics_adc_task(void *arg) {
    (void)arg;

    while (1) {
        uint16_t changed_mask = 0;

        for (uint8_t ch = 0; ch < METRICS_TEMP_CHANNELS; ch++) {
            int16_t raw = 0;
            uint8_t ads_input = g_ads_input_by_temp_channel[ch];
            if (ads1115_read_raw(ads_input, &raw)) {
                g_ads_error = false;
                float temp_c = ads1115_raw_to_temp(raw);
                if (raw < METRICS_RAW_NO_SENSOR_THRESHOLD || !isfinite(temp_c)) {
                    bool was_offline = g_temp_failures[ch] >= METRICS_FAIL_THRESHOLD;
                    g_temp_failures[ch] = METRICS_FAIL_THRESHOLD;
                    if (!was_offline) {
                        ESP_LOGW(METRICS_TAG, "channel %u no sensor (raw=%d temp=%.2f)",
                                 (unsigned)ch, raw, (double)temp_c);
                    }
                    if (metrics_mark_invalid(ch)) {
                        changed_mask |= (uint16_t)(1U << ch);
                    }
                    vTaskDelay(pdMS_TO_TICKS(ADS1115_INTER_CH_DELAY_MS));
                    continue;
                }
                bool was_offline = g_temp_failures[ch] >= METRICS_FAIL_THRESHOLD;
                g_temp_failures[ch] = 0;
                if (was_offline) {
                    ESP_LOGI(METRICS_TAG, "channel %u online", (unsigned)ch);
                }
                if (metrics_update_value(ch, temp_c)) {
                    changed_mask |= (uint16_t)(1U << ch);
                }
            } else {
                if (ads1115_has_error()) {
                    g_ads_error = true;
                    for (uint8_t i = 0; i < METRICS_TEMP_CHANNELS; i++) {
                        if (metrics_mark_invalid(i)) {
                            changed_mask |= (uint16_t)(1U << i);
                        }
                    }
                    break;
                }
                if (g_temp_failures[ch] < 0xFF) {
                    g_temp_failures[ch]++;
                }
                if (g_temp_failures[ch] == METRICS_FAIL_THRESHOLD) {
                    ESP_LOGW(METRICS_TAG, "channel %u offline", (unsigned)ch);
                }
                if (g_temp_failures[ch] >= METRICS_FAIL_THRESHOLD) {
                    if (metrics_mark_invalid(ch)) {
                        changed_mask |= (uint16_t)(1U << ch);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(ADS1115_INTER_CH_DELAY_MS));
        }

        metrics_publish_changes(changed_mask);
        vTaskDelay(pdMS_TO_TICKS(METRICS_ADC_TASK_PERIOD_MS));
    }
}

static void metrics_ina_task(void *arg) {
    (void)arg;

    while (1) {
        uint16_t changed_mask = metrics_update_power_sample();
        metrics_publish_changes(changed_mask);
        vTaskDelay(pdMS_TO_TICKS(METRICS_INA_TASK_PERIOD_MS));
    }
}

static void metrics_tach_task(void *arg) {
    (void)arg;

    uint8_t channel = 0;
    fan_tach_select_channel(channel);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(METRICS_TACH_CHANNEL_SETTLE_MS));

        float rpm = 0.0f;
        uint16_t changed_mask = 0;
        if (metrics_calc_fan_rpm(channel, &rpm)) {
            if (metrics_update_fan(channel, rpm)) {
                changed_mask |= (uint16_t)METRICS_FAN_CHANGED_BIT(channel);
            }
        } else if (metrics_mark_fan_invalid(channel)) {
            changed_mask |= (uint16_t)METRICS_FAN_CHANGED_BIT(channel);
        }

        metrics_publish_changes(changed_mask);

        channel = (uint8_t)((channel + 1U) % METRICS_FAN_CHANNELS);
        fan_tach_select_channel(channel);
        esp_rom_delay_us(FAN_TACH_SWITCH_DELAY_US);
    }
}

static bool metrics_snapshot_read(metrics_snapshot_t *out) {
    if (!out) return false;
    while (1) {
        uint32_t seq1 = __atomic_load_n(&g_snapshot_seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1U) {
            continue;
        }
        metrics_snapshot_t snap = g_snapshot;
        uint32_t seq2 = __atomic_load_n(&g_snapshot_seq, __ATOMIC_ACQUIRE);
        if (seq1 == seq2) {
            *out = snap;
            return true;
        }
    }
}
