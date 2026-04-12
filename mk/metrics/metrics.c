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

#define FAN_TACH_GPIO 18
#define FAN_TACH_PULSES_PER_REV 2
#define FAN_TACH_GLITCH_US 200
#define FAN_TACH_MIN_SAMPLES 4
#define FAN_TACH_AVG_SAMPLES 8
#define FAN_TACH_STOP_TIMEOUT_US 2000000
#define FAN_RPM_NOTIFY_EPS 1.0f
#define TEMP_NOTIFY_EPS 0.01f

static portMUX_TYPE g_fan_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_fan_dt_buf[FAN_TACH_AVG_SAMPLES] = {0};
static volatile uint32_t g_fan_dt_count = 0;
static volatile uint32_t g_fan_dt_idx = 0;
static volatile int64_t g_fan_last_edge_us = 0;
static float g_fan_rpm = NAN;
static bool g_fan_valid = false;
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
    g_fan_last_edge_us = 0;
    g_fan_dt_count = 0;
    g_fan_dt_idx = 0;
    for (int i = 0; i < FAN_TACH_AVG_SAMPLES; i++) {
        g_fan_dt_buf[i] = 0;
    }
    ads1115_init();
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
    int64_t last_edge = 0;
    uint32_t buf[FAN_TACH_AVG_SAMPLES] = {0};
    portENTER_CRITICAL(&g_fan_mux);
    count = g_fan_dt_count;
    last_edge = g_fan_last_edge_us;
    for (uint32_t i = 0; i < FAN_TACH_AVG_SAMPLES; i++) {
        buf[i] = g_fan_dt_buf[i];
    }
    portEXIT_CRITICAL(&g_fan_mux);

    if (last_edge == 0) {
        return false;
    }
    if ((now - last_edge) > FAN_TACH_STOP_TIMEOUT_US) {
        *out_rpm = 0.0f;
        return true;
    }
    if (count < FAN_TACH_MIN_SAMPLES) {
        return false;
    }
    if (count > FAN_TACH_AVG_SAMPLES) {
        count = FAN_TACH_AVG_SAMPLES;
    }
    uint32_t values[FAN_TACH_AVG_SAMPLES];
    uint32_t used = 0;
    for (uint32_t i = 0; i < FAN_TACH_AVG_SAMPLES && used < count; i++) {
        if (buf[i] > 0) {
            values[used++] = buf[i];
        }
    }
    if (used < FAN_TACH_MIN_SAMPLES) {
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
    uint32_t median_dt = values[used / 2];
    if (median_dt == 0) {
        return false;
    }
    *out_rpm = (60.0f * 1000000.0f) / (median_dt * (float)FAN_TACH_PULSES_PER_REV);
    return true;
}

static void IRAM_ATTR fan_tach_isr(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&g_fan_mux);
    int64_t last = g_fan_last_edge_us;
    if (last != 0) {
        uint32_t dt = (uint32_t)(now - last);
        if (dt >= FAN_TACH_GLITCH_US) {
            uint32_t idx = g_fan_dt_idx;
            g_fan_dt_buf[idx] = dt;
            g_fan_dt_idx = (idx + 1) % FAN_TACH_AVG_SAMPLES;
            if (g_fan_dt_count < FAN_TACH_AVG_SAMPLES) {
                g_fan_dt_count++;
            }
        }
    }
    g_fan_last_edge_us = now;
    portEXIT_CRITICAL_ISR(&g_fan_mux);
}

static bool metrics_update_fan(float rpm) {
    if (!isfinite(rpm) || rpm <= 0.0f) {
        if (!g_fan_valid) return false;
        g_fan_valid = false;
        g_fan_rpm = NAN;
        return true;
    }
    if (!g_fan_valid || fabsf(g_fan_rpm - rpm) >= FAN_RPM_NOTIFY_EPS) {
        g_fan_valid = true;
        g_fan_rpm = rpm;
        return true;
    }
    return false;
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
    }

    metrics_snapshot_write_from_state();
    return changed_mask;
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
