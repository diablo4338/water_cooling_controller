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

static const char *METRICS_TAG = "metrics";

static float g_temp_values[METRICS_TEMP_CHANNELS] = {NAN, NAN, NAN, NAN};
static bool g_temp_valid[METRICS_TEMP_CHANNELS] = {false};
static uint8_t g_temp_failures[METRICS_TEMP_CHANNELS] = {0};
static bool g_metrics_error = false;

#define FAN_TACH_GPIO 18
#define FAN_TACH_PULSES_PER_REV 2
#define FAN_TACH_MIN_VALID_DT_US 10000U
#define FAN_TACH_DT_BUF_SIZE 16
#define FAN_TACH_MIN_CALC_SAMPLES 6
#define FAN_TACH_TRIM_SAMPLES 2
#define FAN_TACH_RATIO_SCALE 10U
#define FAN_TACH_MIN_RATIO_X10 6U
#define FAN_TACH_MAX_RATIO_X10 22U
#define FAN_TACH_STOP_TIMEOUT_US 2000000
#define FAN_RPM_NOTIFY_EPS 1.0f
#define FAN_RPM_EMA_OLD 0.7f
#define FAN_RPM_EMA_NEW 0.3f
#define TEMP_NOTIFY_EPS 0.01f

static portMUX_TYPE g_fan_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_fan_dt_buf[FAN_TACH_DT_BUF_SIZE] = {0};
static volatile uint32_t g_fan_dt_count = 0;
static volatile uint32_t g_fan_dt_idx = 0;
static volatile int64_t g_fan_last_edge_us = 0;
static volatile int64_t g_fan_last_valid_edge_us = 0;
static volatile uint32_t g_fan_last_good_dt_us = 0;
static float g_fan_rpm = NAN;
static bool g_fan_valid = false;
static float g_fan_filtered_rpm = 0.0f;
static bool g_fan_filter_valid = false;
static uint32_t g_snapshot_seq = 0;
static metrics_snapshot_t g_snapshot = {0};

static void fan_pcnt_init(void);
static void fan_tach_isr(void *arg);
static void metrics_snapshot_write_from_state(void);
static bool metrics_snapshot_read(metrics_snapshot_t *out);

#define METRICS_FAIL_THRESHOLD 3
#define METRICS_RAW_NO_SENSOR_THRESHOLD 500

void metrics_init(void) {
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        g_temp_values[i] = NAN;
        g_temp_valid[i] = false;
        g_temp_failures[i] = 0;
    }
    g_fan_rpm = NAN;
    g_fan_valid = false;
    g_fan_filtered_rpm = 0.0f;
    g_fan_filter_valid = false;
    g_fan_last_edge_us = 0;
    g_fan_last_valid_edge_us = 0;
    g_fan_last_good_dt_us = 0;
    g_fan_dt_count = 0;
    g_fan_dt_idx = 0;
    for (int i = 0; i < FAN_TACH_DT_BUF_SIZE; i++) {
        g_fan_dt_buf[i] = 0;
    }
    g_metrics_error = !ads1115_init();
    fan_pcnt_init();
    metrics_snapshot_write_from_state();
}

float metrics_get_temp(uint8_t channel) {
    if (channel >= METRICS_TEMP_CHANNELS) return NAN;
    metrics_snapshot_t snap;
    metrics_get_snapshot(&snap);
    if (!snap.temp_valid[channel]) return NAN;
    return snap.temp_c[channel];
}

float metrics_get_fan_speed_rpm(void) {
    metrics_snapshot_t snap;
    metrics_get_snapshot(&snap);
    if (!snap.fan_valid) return NAN;
    return snap.fan_rpm;
}

static void fan_pcnt_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FAN_TACH_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
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

static bool metrics_update_value(uint8_t channel, float temp_c) {
    if (channel >= METRICS_TEMP_CHANNELS) return false;
    if (!isfinite(temp_c)) return false;

    if (!g_temp_valid[channel] || fabsf(g_temp_values[channel] - temp_c) >= TEMP_NOTIFY_EPS) {
        g_temp_values[channel] = temp_c;
        g_temp_valid[channel] = true;
        return true;
    }
    return false;
}

static bool metrics_mark_invalid(uint8_t channel) {
    if (channel >= METRICS_TEMP_CHANNELS) return false;
    if (!g_temp_valid[channel]) return false;
    g_temp_values[channel] = NAN;
    g_temp_valid[channel] = false;
    return true;
}

static bool metrics_calc_fan_rpm(float *out_rpm) {
    int64_t now = esp_timer_get_time();
    uint32_t count = 0;
    int64_t last_valid_edge = 0;
    uint32_t buf[FAN_TACH_DT_BUF_SIZE] = {0};
    bool stopped = false;

    portENTER_CRITICAL(&g_fan_mux);
    last_valid_edge = g_fan_last_valid_edge_us;
    if (last_valid_edge != 0 && (now - last_valid_edge) > FAN_TACH_STOP_TIMEOUT_US) {
        stopped = true;
        g_fan_dt_count = 0;
        g_fan_dt_idx = 0;
        g_fan_last_edge_us = 0;
        g_fan_last_good_dt_us = 0;
        for (uint32_t i = 0; i < FAN_TACH_DT_BUF_SIZE; i++) {
            g_fan_dt_buf[i] = 0;
        }
    } else {
        count = g_fan_dt_count;
        for (uint32_t i = 0; i < FAN_TACH_DT_BUF_SIZE; i++) {
            buf[i] = g_fan_dt_buf[i];
        }
    }
    portEXIT_CRITICAL(&g_fan_mux);

    if (last_valid_edge == 0) {
        return false;
    }
    if (stopped) {
        g_fan_filtered_rpm = 0.0f;
        g_fan_filter_valid = false;
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
    if (!g_fan_filter_valid) {
        g_fan_filtered_rpm = raw_rpm;
        g_fan_filter_valid = true;
    } else {
        g_fan_filtered_rpm = g_fan_filtered_rpm * FAN_RPM_EMA_OLD + raw_rpm * FAN_RPM_EMA_NEW;
    }
    *out_rpm = g_fan_filtered_rpm;
    return true;
}

static void IRAM_ATTR fan_tach_isr(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&g_fan_mux);
    int64_t last = g_fan_last_edge_us;
    g_fan_last_edge_us = now;
    if (last == 0) {
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }
    int64_t dt64 = now - last;
    if (dt64 <= 0) {
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }
    uint32_t dt_us = (uint32_t)dt64;
    if (dt_us < FAN_TACH_MIN_VALID_DT_US) {
        portEXIT_CRITICAL_ISR(&g_fan_mux);
        return;
    }

    uint32_t last_good_dt_us = g_fan_last_good_dt_us;
    if (last_good_dt_us != 0) {
        uint64_t dt_scaled = (uint64_t)dt_us * FAN_TACH_RATIO_SCALE;
        if (dt_scaled < (uint64_t)last_good_dt_us * FAN_TACH_MIN_RATIO_X10 ||
            dt_scaled > (uint64_t)last_good_dt_us * FAN_TACH_MAX_RATIO_X10) {
            portEXIT_CRITICAL_ISR(&g_fan_mux);
            return;
        }
    }

    g_fan_last_valid_edge_us = now;
    g_fan_last_good_dt_us = dt_us;
    uint32_t idx = g_fan_dt_idx;
    g_fan_dt_buf[idx] = dt_us;
    g_fan_dt_idx = (idx + 1) % FAN_TACH_DT_BUF_SIZE;
    if (g_fan_dt_count < FAN_TACH_DT_BUF_SIZE) {
        g_fan_dt_count++;
    }
    portEXIT_CRITICAL_ISR(&g_fan_mux);
}

static bool metrics_update_fan(float rpm) {
    if (!isfinite(rpm) || rpm < 0.0f) {
        if (!g_fan_valid) return false;
        g_fan_valid = false;
        return true;
    }
    bool changed = !g_fan_valid || fabsf(g_fan_rpm - rpm) >= FAN_RPM_NOTIFY_EPS;
    g_fan_valid = true;
    g_fan_rpm = rpm;
    return changed;
}

static bool metrics_mark_fan_invalid(void) {
    if (!g_fan_valid) return false;
    g_fan_valid = false;
    g_fan_filter_valid = false;
    g_fan_filtered_rpm = 0.0f;
    return true;
}

void metrics_get_snapshot(metrics_snapshot_t *out) {
    if (!out) return;
    metrics_snapshot_read(out);
}

uint8_t metrics_sample_all(void) {
    uint8_t changed_mask = 0;

    for (uint8_t ch = 0; ch < METRICS_TEMP_CHANNELS; ch++) {
        int16_t raw = 0;
        if (ads1115_read_raw(ch, &raw)) {
            g_metrics_error = false;
            if (raw < METRICS_RAW_NO_SENSOR_THRESHOLD) {
                bool was_offline = g_temp_failures[ch] >= METRICS_FAIL_THRESHOLD;
                g_temp_failures[ch] = METRICS_FAIL_THRESHOLD;
                if (!was_offline) {
                    ESP_LOGW(METRICS_TAG, "channel %u no sensor (raw=%d)", (unsigned)ch, raw);
                }
                if (metrics_mark_invalid(ch)) {
                    changed_mask |= (uint8_t)(1U << ch);
                }
                vTaskDelay(pdMS_TO_TICKS(ADS1115_INTER_CH_DELAY_MS));
                continue;
            }
            bool was_offline = g_temp_failures[ch] >= METRICS_FAIL_THRESHOLD;
            g_temp_failures[ch] = 0;
            if (was_offline) {
                ESP_LOGI(METRICS_TAG, "channel %u online", (unsigned)ch);
            }
            float temp_c = ads1115_raw_to_temp(raw);
            if (metrics_update_value(ch, temp_c)) {
                changed_mask |= (uint8_t)(1U << ch);
            }
        } else {
            if (ads1115_has_error()) {
                g_metrics_error = true;
                for (uint8_t i = 0; i < METRICS_TEMP_CHANNELS; i++) {
                    if (metrics_mark_invalid(i)) {
                        changed_mask |= (uint8_t)(1U << i);
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
                    changed_mask |= (uint8_t)(1U << ch);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ADS1115_INTER_CH_DELAY_MS));
    }

    float rpm = 0.0f;
    if (metrics_calc_fan_rpm(&rpm)) {
        if (metrics_update_fan(rpm)) {
            changed_mask |= METRICS_FAN_CHANGED_BIT;
        }
    } else if (metrics_mark_fan_invalid()) {
        changed_mask |= METRICS_FAN_CHANGED_BIT;
    }

    metrics_snapshot_write_from_state();
    return changed_mask;
}

bool metrics_has_error(void) {
    return g_metrics_error;
}

static void metrics_snapshot_write_from_state(void) {
    metrics_snapshot_t snap;
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        snap.temp_c[i] = g_temp_values[i];
        snap.temp_valid[i] = g_temp_valid[i] ? 1U : 0U;
    }
    snap.fan_rpm = g_fan_rpm;
    snap.fan_valid = g_fan_valid ? 1U : 0U;

    __atomic_fetch_add(&g_snapshot_seq, 1U, __ATOMIC_ACQ_REL);
    g_snapshot = snap;
    __atomic_fetch_add(&g_snapshot_seq, 1U, __ATOMIC_ACQ_REL);
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
