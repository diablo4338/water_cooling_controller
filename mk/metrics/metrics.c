#include "metrics.h"

#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ads1115.h"

static float g_temp_values[4] = {NAN, NAN, NAN, NAN};
static bool g_temp_valid[4] = {false};

void metrics_init(void) {
    for (int i = 0; i < 4; i++) {
        g_temp_values[i] = NAN;
        g_temp_valid[i] = false;
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

uint8_t metrics_sample_all(void) {
    uint8_t changed_mask = 0;

    for (uint8_t ch = 0; ch < 4; ch++) {
        int16_t raw = 0;
        if (ads1115_read_raw(ch, &raw)) {
            float temp_c = ads1115_raw_to_temp(raw);
            if (metrics_update_value(ch, temp_c)) {
                changed_mask |= (uint8_t)(1U << ch);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ADS1115_INTER_CH_DELAY_MS));
    }

    return changed_mask;
}
