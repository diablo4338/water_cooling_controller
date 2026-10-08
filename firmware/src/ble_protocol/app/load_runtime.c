#include "load_runtime.h"

#include <math.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ads1115.h"
#include "ina226.h"
#include "load_output.h"
#include "load_state.h"
#include "buzzer_logic.h"
#include "pair_mode.h"
#include "state.h"
#include "gap.h"
#include "connect_window.h"
#include "load_metrics_ble.h"
#include "storage.h"
#include "runtime_config.h"

#define RETAINED_MAGIC 0x4C4F4106U
/* Physical ADS input 2 maps to logical temperature channel 4. */
#define BUTTON_ADS_INPUT 2
#define PAIR_BUTTON_GPIO GPIO_NUM_19
#define PAIR_BUTTON_ACTIVE_LEVEL 0

static portMUX_TYPE window_mux = portMUX_INITIALIZER_UNLOCKED;
static connect_window_t connect_window;
static bool window_expiry_logged = true;
static portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE pair_button_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool pair_button_irq_pending;
static bool pairing_request_pending;
static load_runtime_snapshot_t snapshot = {
    .temperature_c = NAN,
    .voltage_v = NAN,
    .current_ma = NAN,
};

#define BUTTON_LED_GPIO GPIO_NUM_1
#define BUZZER_GPIO GPIO_NUM_0

static void buzzer_signal(uint16_t duration_ms) {
    if (!duration_ms) return;
    ESP_ERROR_CHECK(gpio_set_level(BUZZER_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ESP_ERROR_CHECK(gpio_set_level(BUZZER_GPIO, 0));
}

static void IRAM_ATTR pair_button_isr(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&pair_button_mux);
    pair_button_irq_pending = true;
    portEXIT_CRITICAL_ISR(&pair_button_mux);
}

static bool take_pair_button_press(void) {
    portENTER_CRITICAL(&pair_button_mux);
    bool pressed = pair_button_irq_pending;
    pair_button_irq_pending = false;
    portEXIT_CRITICAL(&pair_button_mux);
    return pressed;
}

void load_runtime_open_window(void) {
    portENTER_CRITICAL(&window_mux);
    bool restarting = connect_window_remaining_ms(&connect_window, esp_timer_get_time()) > 0;
    connect_window_open(&connect_window, esp_timer_get_time());
    window_expiry_logged = false;
    int32_t remaining = connect_window_remaining_ms(&connect_window, esp_timer_get_time());
    portEXIT_CRITICAL(&window_mux);
    ESP_LOGI("load", "Timer connect_window %s duration_ms=%ld",
             restarting ? "restarted" : "started", (long)remaining);
}

void load_runtime_close_window(void) {
    portENTER_CRITICAL(&window_mux);
    bool active = connect_window_remaining_ms(&connect_window, esp_timer_get_time()) > 0;
    connect_window.deadline_us = 0;
    window_expiry_logged = true;
    portEXIT_CRITICAL(&window_mux);
    if (active) ESP_LOGI("load", "Timer connect_window cancelled reason=disconnected");
}

int32_t load_runtime_window_remaining_ms(void) {
    portENTER_CRITICAL(&window_mux);
    int32_t remaining = connect_window_remaining_ms(&connect_window, esp_timer_get_time());
    bool expired = remaining == 0 && !window_expiry_logged;
    if (expired) window_expiry_logged = true;
    portEXIT_CRITICAL(&window_mux);
    if (expired) ESP_LOGI("load", "Timer connect_window expired");
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
    buzzer_state_t buzzer;
    bool reset_button_was_down;
} retained;

void load_runtime_init(void) {
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP || retained.magic != RETAINED_MAGIC ||
        (retained.state.mode != LOAD_IDLE && retained.state.mode != LOAD_WORKING) ||
        retained.state.temperature_count > 5 || retained.state.temperature_next >= 5) {
        retained.state = (load_state_t){.mode = LOAD_IDLE};
        retained.buzzer = (buzzer_state_t){0};
        retained.reset_button_was_down = false;
        retained.magic = RETAINED_MAGIC;
    }
    load_output_restore(retained.state.mode == LOAD_WORKING);
    ESP_ERROR_CHECK(gpio_set_level(BUZZER_GPIO, 0));
    const gpio_config_t buzzer = {
        .pin_bit_mask = 1ULL << BUZZER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&buzzer));
    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << BUTTON_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    ESP_ERROR_CHECK(gpio_set_level(BUTTON_LED_GPIO, retained.state.button_was_pressed));

    const gpio_config_t pair_button = {
        .pin_bit_mask = 1ULL << PAIR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&pair_button));
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(PAIR_BUTTON_GPIO, pair_button_isr, NULL));
    if (gpio_get_level(PAIR_BUTTON_GPIO) != PAIR_BUTTON_ACTIVE_LEVEL) {
        retained.reset_button_was_down = false;
    } else if (!retained.reset_button_was_down) {
        portENTER_CRITICAL(&pair_button_mux);
        pair_button_irq_pending = true;
        portEXIT_CRITICAL(&pair_button_mux);
    }
}

void load_runtime_process_pair_button(void) {
    portENTER_CRITICAL(&pair_button_mux);
    bool requested = pairing_request_pending;
    pairing_request_pending = false;
    portEXIT_CRITICAL(&pair_button_mux);
    if (!requested) return;

    ESP_LOGI("load", "GPIO19 button pressed action=global_reset");
    esp_err_t config_err = runtime_config_reset_defaults();
    if (config_err != ESP_OK) {
        ESP_LOGE("load", "Global reset: config defaults not saved: %s",
                 esp_err_to_name(config_err));
    }
    retained.buzzer = (buzzer_state_t){0};
    trust_reset();
    pair_mode_activate();
    gap_disconnect_client();
}

void load_runtime_get_snapshot(load_runtime_snapshot_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&snapshot_mux);
    *out = snapshot;
    portEXIT_CRITICAL(&snapshot_mux);
}

void load_runtime_cycle(void) {
    runtime_config_t config;
    runtime_config_get(&config);
    const load_policy_t policy = {
        .button_delta_c = (float)config.button_delta_deci_c / 10.0f,
        .idle_current_ma = (float)config.idle_current_ma,
        .idle_timeout_us = (int64_t)config.idle_timeout_s * 1000000LL,
        .require_new_press = true,
    };
    int16_t raw = 0;
    float temperature = NAN;
    if (ads1115_init() && ads1115_read_raw(BUTTON_ADS_INPUT, &raw) && raw >= 500) {
        temperature = ads1115_raw_to_temp(raw);
    }
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t now_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
    bool pressed = load_button_is_pressed(&retained.state, &policy, temperature);

    if (take_pair_button_press()) {
        retained.reset_button_was_down = true;
        load_runtime_open_window();
        portENTER_CRITICAL(&pair_button_mux);
        pairing_request_pending = true;
        portEXIT_CRITICAL(&pair_button_mux);
    }
    if (gpio_get_level(PAIR_BUTTON_GPIO) != PAIR_BUTTON_ACTIVE_LEVEL) {
        retained.reset_button_was_down = false;
    }

    /* The thermal button only opens the BLE window and drives the load FSM. */
    if (pressed && !retained.state.button_was_pressed) {
        load_runtime_open_window();
    }
    ina226_sample_t sample;
    bool ina_valid = ina226_get_single_sample(&sample);
    float voltage = ina_valid ? sample.voltage_v : NAN;
    float current = ina_valid ? sample.current_ma : NAN;
    load_state_t previous = retained.state;
    load_state_step(&retained.state, &policy, temperature, current, now_us);
    const buzzer_policy_t buzzer_policy = {
        .enabled = config.buzzer_enabled != 0,
        .threshold_mv = config.buzzer_threshold_mv,
        .low_delay_s = config.buzzer_low_delay_s,
        .button_ms = config.buzzer_button_ms,
        .low_ms = config.buzzer_low_ms,
        .combined_ms = config.buzzer_combined_ms,
    };
    uint16_t beep_ms = buzzer_step(&retained.buzzer, &buzzer_policy,
                                   pressed && !previous.button_was_pressed,
                                   voltage, now_us);
    if (!previous.low_current_pending && retained.state.low_current_pending) {
        ESP_LOGI("load", "Timer low_current started duration_ms=%lld current_ma=%.1f",
                 (long long)(policy.idle_timeout_us / 1000), (double)current);
    } else if (previous.low_current_pending && !retained.state.low_current_pending) {
        if (retained.state.mode != previous.mode) {
            ESP_LOGI("load", "Timer low_current expired");
        } else {
            ESP_LOGI("load", "Timer low_current cancelled reason=%s current_ma=%.1f",
                     isfinite(current) ? "current_recovered" : "invalid_sample", (double)current);
        }
    }
    if (previous.mode != retained.state.mode) {
        ESP_LOGI("load", "State %s -> %s reason=%s",
                 previous.mode == LOAD_IDLE ? "IDLE" : "WORKING",
                 retained.state.mode == LOAD_IDLE ? "IDLE" : "WORKING",
                 retained.state.mode == LOAD_IDLE ? "low_current_timeout" : "button_press");
    }
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
    buzzer_signal(beep_ms);
}

void load_runtime_sleep(void) {
    runtime_config_t config;
    runtime_config_get(&config);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)config.deep_sleep_ms * 1000ULL));
    if (gpio_get_level(PAIR_BUTTON_GPIO) != PAIR_BUTTON_ACTIVE_LEVEL) {
        ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(
            1ULL << PAIR_BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));
    }
    load_output_hold();
    esp_deep_sleep_start();
}

void load_runtime_task(void *arg) {
    (void)arg;
    for (;;) {
        load_runtime_cycle();
        load_runtime_process_pair_button();
        /* GAP owns the final sleep decision, serialized with connection events.
         * This blocks until GAP either enters sleep or declines the request. */
        gap_service_poll();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
