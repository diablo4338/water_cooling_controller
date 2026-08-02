#include "overheat_alarm.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_status.h"
#include "params.h"

#define OVERHEAT_ALARM_GPIO 0
#define OVERHEAT_ALARM_ON_LEVEL 1
#define OVERHEAT_ALARM_OFF_LEVEL 0
#define OVERHEAT_ALARM_PHASE_MS 2000
#define OVERHEAT_ALARM_POLL_MS 20

static volatile bool s_silence_requested = false;

bool overheat_alarm_update(overheat_alarm_state_t *state,
                           bool alarm_enabled,
                           bool overheat_active,
                           bool silence_requested) {
    if (!state) {
        return false;
    }

    if (!overheat_active) {
        state->silenced = false;
        return false;
    }
    if (silence_requested) {
        state->silenced = true;
    }
    return alarm_enabled && !state->silenced;
}

void overheat_alarm_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << OVERHEAT_ALARM_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(OVERHEAT_ALARM_GPIO, OVERHEAT_ALARM_OFF_LEVEL);
    __atomic_store_n(&s_silence_requested, false, __ATOMIC_RELEASE);
}

void overheat_alarm_silence(void) {
    __atomic_store_n(&s_silence_requested, true, __ATOMIC_RELEASE);
}

void overheat_alarm_task(void *param) {
    (void)param;

    overheat_alarm_state_t state = {0};
    bool output_on = false;
    bool was_sounding = false;
    TickType_t last_phase_tick = xTaskGetTickCount();

    while (1) {
        bool silence_requested =
            __atomic_exchange_n(&s_silence_requested, false, __ATOMIC_ACQ_REL);
        params_t params;
        bool alarm_enabled = params_cache_get(&params) && params.overheat_alarm_enabled != 0;
        bool sounding = overheat_alarm_update(
            &state,
            alarm_enabled,
            device_status_has_error_flag(DEVICE_ERROR_OVERHEAT),
            silence_requested
        );
        TickType_t now = xTaskGetTickCount();

        if (sounding && !was_sounding) {
            output_on = true;
            last_phase_tick = now;
        } else if (!sounding) {
            output_on = false;
        } else if ((now - last_phase_tick) >= pdMS_TO_TICKS(OVERHEAT_ALARM_PHASE_MS)) {
            output_on = !output_on;
            last_phase_tick = now;
        }

        gpio_set_level(
            OVERHEAT_ALARM_GPIO,
            output_on ? OVERHEAT_ALARM_ON_LEVEL : OVERHEAT_ALARM_OFF_LEVEL
        );
        was_sounding = sounding;
        vTaskDelay(pdMS_TO_TICKS(OVERHEAT_ALARM_POLL_MS));
    }
}
