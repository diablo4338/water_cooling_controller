#include "metrics.h"

#include <math.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ads1115.h"

static const char *METRICS_TAG = "metrics";

static float g_temp_values[4] = {NAN, NAN, NAN, NAN};
static bool g_temp_valid[4] = {false};
static uint8_t g_temp_failures[4] = {0};

#define METRICS_FAIL_THRESHOLD 3
#define METRICS_RAW_NO_SENSOR_THRESHOLD 500

void metrics_init(void) {
    for (int i = 0; i < 4; i++) {
        g_temp_values[i] = NAN;
        g_temp_valid[i] = false;
        g_temp_failures[i] = 0;
    }
    ads1115_init();
}

float metrics_get_temp(uint8_t channel) {
    if (channel > 3 || !g_temp_valid[channel]) return NAN;
    return g_temp_values[channel];
}

static bool metrics_update_value(uint8_t channel, float temp_c) {
    if (channel > 3) return false;
    if (!isfinite(temp_c)) return false;

    if (!g_temp_valid[channel] || g_temp_values[channel] != temp_c) {
        g_temp_values[channel] = temp_c;
        g_temp_valid[channel] = true;
        return true;
    }
    return false;
}

static bool metrics_mark_invalid(uint8_t channel) {
    if (channel > 3) return false;
    if (!g_temp_valid[channel]) return false;
    g_temp_values[channel] = NAN;
    g_temp_valid[channel] = false;
    return true;
}

uint8_t metrics_sample_all(void) {
    uint8_t changed_mask = 0;

    for (uint8_t ch = 0; ch < 4; ch++) {
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

    return changed_mask;
}
