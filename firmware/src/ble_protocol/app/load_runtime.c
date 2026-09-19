#include "load_runtime.h"

#include <math.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ads1115.h"
#include "ina226.h"
#include "load_output.h"
#include "load_state.h"
#include "pair_mode.h"
#include "state.h"
#include "gap.h"
#include "connect_window.h"
#include "load_metrics_ble.h"
#include "storage.h"

#define RETAINED_MAGIC 0x4C4F4102U
/* Physical ADS input 2 maps to logical temperature channel 4. */
#define BUTTON_ADS_INPUT 2

static portMUX_TYPE window_mux = portMUX_INITIALIZER_UNLOCKED;
static connect_window_t connect_window;
static portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static load_runtime_snapshot_t snapshot = {
    .temperature_c = NAN,
    .voltage_v = NAN,
    .current_ma = NAN,
};

#define BUTTON_LED_GPIO GPIO_NUM_1
#define LONG_PRESS_US (10LL * 1000000)

void load_runtime_open_window(void) {
    portENTER_CRITICAL(&window_mux);
    connect_window_open(&connect_window, esp_timer_get_time());
    portEXIT_CRITICAL(&window_mux);
}

int32_t load_runtime_window_remaining_ms(void) {
    portENTER_CRITICAL(&window_mux);
    int32_t remaining = connect_window_remaining_ms(&connect_window, esp_timer_get_time());
    portEXIT_CRITICAL(&window_mux);
    return remaining;
}

bool load_runtime_connection_blocks_sleep(bool connected) {
    portENTER_CRITICAL(&window_mux);
    bool blocked = connect_window_blocks_sleep(&connect_window, esp_timer_get_time(), connected);
    portEXIT_CRITICAL(&window_mux);
    return blocked;
}

static RTC_DATA_ATTR struct {
    uint32_t magic;
    load_state_t state;
    bool long_press_pending;
    bool reset_sent;
    int64_t long_press_since_us;
} retained;

static const load_policy_t policy = {
    .button_temperature_c = 30.0f,
    .idle_current_ma = 100.0f,
    .idle_timeout_us = 300LL * 1000000,
    .require_new_press = true,
};

void load_runtime_init(void) {
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP || retained.magic != RETAINED_MAGIC ||
        (retained.state.mode != LOAD_IDLE && retained.state.mode != LOAD_WORKING)) {
        retained.state = (load_state_t){.mode = LOAD_IDLE};
        retained.magic = RETAINED_MAGIC;
    }
    load_output_restore(retained.state.mode == LOAD_WORKING);
    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << BUTTON_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    ESP_ERROR_CHECK(gpio_set_level(BUTTON_LED_GPIO, retained.state.button_was_pressed));
}

void load_runtime_get_snapshot(load_runtime_snapshot_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&snapshot_mux);
    *out = snapshot;
    portEXIT_CRITICAL(&snapshot_mux);
}

void load_runtime_cycle(void) {
    int16_t raw = 0;
    float temperature = NAN;
    if (ads1115_init() && ads1115_read_raw(BUTTON_ADS_INPUT, &raw) && raw >= 500) {
        temperature = ads1115_raw_to_temp(raw);
    }
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t now_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
    bool valid_button = isfinite(temperature);
    bool pressed = valid_button && temperature > policy.button_temperature_c;

    /* Read the edge before the load FSM updates its retained button history.
     * The BLE window also opens in WORKING, without changing the load state. */
    if (pressed && !retained.state.button_was_pressed) {
        load_runtime_open_window();
        retained.long_press_pending = true;
        retained.reset_sent = false;
        retained.long_press_since_us = now_us;
    } else if (valid_button && !pressed) {
        retained.long_press_pending = false;
        retained.reset_sent = false;
    }
    if (pressed && retained.long_press_pending && !retained.reset_sent &&
        now_us - retained.long_press_since_us >= LONG_PRESS_US) {
        retained.reset_sent = true;
        trust_reset();
        pair_mode_activate();
        gap_disconnect_client();
    }
    ina226_sample_t sample;
    bool ina_valid = ina226_get_single_sample(&sample);
    float voltage = ina_valid ? sample.voltage_v : NAN;
    float current = ina_valid ? sample.current_ma : NAN;
    load_state_step(&retained.state, &policy, temperature, current, now_us);
    load_output_set(retained.state.mode == LOAD_WORKING);
    ESP_ERROR_CHECK(gpio_set_level(BUTTON_LED_GPIO, retained.state.button_was_pressed));

    portENTER_CRITICAL(&snapshot_mux);
    snapshot.load_state = (uint8_t)retained.state.mode;
    snapshot.button_pressed = retained.state.button_was_pressed ? 1U : 0U;
    snapshot.temperature_c = temperature;
    snapshot.voltage_v = voltage;
    snapshot.current_ma = current;
    portEXIT_CRITICAL(&snapshot_mux);
    load_metrics_publish();
}

void load_runtime_sleep(void) {
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(1000000ULL));
    load_output_hold();
    esp_deep_sleep_start();
}

void load_runtime_task(void *arg) {
    (void)arg;
    for (;;) {
        load_runtime_cycle();
        /* GAP owns the final sleep decision, serialized with connection events.
         * This blocks until GAP either enters sleep or declines the request. */
        gap_service_poll();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
