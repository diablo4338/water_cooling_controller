#include "load_runtime.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "action_executor.h"
#include "ads1115.h"
#include "buzzer_fsm.h"
#include "connect_window.h"
#include "event_bus.h"
#include "gap.h"
#include "ina226.h"
#include "load_fsm.h"
#include "load_metrics_ble.h"
#include "load_output.h"
#include "load_state.h"
#include "pair_mode.h"
#include "power_manager.h"
#include "runtime_config.h"
#include "runtime_core.h"
#include "runtime_events.h"
#include "state.h"
#include "storage.h"

#define TAG "runtime"

#define RETAINED_MAGIC 0x4C4F4108U
/* Physical ADS input 2 maps to logical temperature channel 4. */
#define BUTTON_ADS_INPUT 2
#define PAIR_BUTTON_GPIO GPIO_NUM_19
#define PAIR_BUTTON_ACTIVE_LEVEL 0
#define BUTTON_LED_GPIO GPIO_NUM_1

#define PERIODIC_INTERVAL_US (1000LL * 1000LL)
#define DEADLINE_CHECK_INTERVAL_US (250LL * 1000LL)
#define SLEEP_CHECK_INTERVAL_US (250LL * 1000LL)
#define BOOT_MEASUREMENT_TIMEOUT_US (10LL * 1000LL * 1000LL)
#define DISPATCH_RECEIVE_TIMEOUT_MS 50U
#define DISPATCH_LOCK_TIMEOUT_MS 100U

typedef struct {
    uint32_t magic;
    load_state_t load;
    bool buzzer_low_pending;
    bool buzzer_low_alerted;
    int64_t buzzer_low_since_us;
    bool reset_button_was_down;
} retained_state_t;

static RTC_DATA_ATTR retained_state_t s_retained;

static runtime_core_t s_core;
static load_fsm_config_t s_load_config;
static buzzer_fsm_config_t s_buzzer_config;
static ble_fsm_config_t s_ble_config;

static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static load_runtime_snapshot_t s_snapshot = {
    .temperature_c = NAN,
    .voltage_v = NAN,
    .current_ma = NAN,
};

static portMUX_TYPE s_pair_button_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pair_button_irq_pending;

static bool s_boot_measurement_pending;
static bool s_event_bus_fault_reported;
static int64_t s_boot_started_us;
static bool s_periodic_pending;
static uint32_t s_periodic_request_id;
static bool s_sleep_requested;
static int64_t s_next_periodic_us;
static int64_t s_next_deadline_us;
static int64_t s_next_sleep_us;

static int64_t wall_time_us(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_usec;
}

static void IRAM_ATTR pair_button_isr(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&s_pair_button_mux);
    s_pair_button_irq_pending = true;
    portEXIT_CRITICAL_ISR(&s_pair_button_mux);
}

static bool take_pair_button_press(void) {
    portENTER_CRITICAL(&s_pair_button_mux);
    bool pressed = s_pair_button_irq_pending;
    s_pair_button_irq_pending = false;
    portEXIT_CRITICAL(&s_pair_button_mux);
    return pressed;
}

static bool pair_button_irq_pending(void) {
    portENTER_CRITICAL(&s_pair_button_mux);
    bool pressed = s_pair_button_irq_pending;
    portEXIT_CRITICAL(&s_pair_button_mux);
    return pressed;
}

static void refresh_configs(void) {
    runtime_config_t config;
    runtime_config_get(&config);

    s_load_config.low_current_ma = (float)config.idle_current_ma;
    s_load_config.low_current_timeout_us =
        (int64_t)config.idle_timeout_s * 1000000LL;

    s_buzzer_config = (buzzer_fsm_config_t) {
        .enabled = config.buzzer_enabled != 0,
        .threshold_mv = config.buzzer_threshold_mv,
        .low_delay_s = config.buzzer_low_delay_s,
        .button_ms = config.buzzer_button_ms,
        .low_ms = config.buzzer_low_ms,
        .combined_ms = config.buzzer_combined_ms,
        .settle_ms = BUZZER_SETTLE_MS,
        .measure_timeout_ms = BUZZER_MEASURE_TIMEOUT_MS,
        .error_on_ms = BUZZER_ERROR_ON_MS,
        .error_off_ms = BUZZER_ERROR_OFF_MS,
        .error_repeats = BUZZER_ERROR_REPEATS,
    };

    s_ble_config.window_us = CONNECT_WINDOW_US;
    s_ble_config.max_stop_retries = 2;

    s_core.load_config = s_load_config;
    s_core.buzzer_config = s_buzzer_config;
    s_core.ble_config = s_ble_config;
}

static void retained_init(void) {
    bool valid = esp_reset_reason() == ESP_RST_DEEPSLEEP &&
                 s_retained.magic == RETAINED_MAGIC &&
                 (s_retained.load.mode == LOAD_IDLE ||
                  s_retained.load.mode == LOAD_WORKING) &&
                 s_retained.load.temperature_count <= 5 &&
                 s_retained.load.temperature_next < 5 &&
                 (!s_retained.load.low_current_pending ||
                  s_retained.load.low_current_since_us > 0);

    if (!valid) {
        memset(&s_retained, 0, sizeof(s_retained));
        s_retained.magic = RETAINED_MAGIC;
        s_retained.load.mode = LOAD_IDLE;
    }
}

static void snapshot_update_load(void) {
    portENTER_CRITICAL(&s_snapshot_mux);
    s_snapshot.load_state = s_core.load.output_on ? LOAD_WORKING : LOAD_IDLE;
    s_snapshot.button_pressed = s_retained.load.button_was_pressed ? 1U : 0U;
    portEXIT_CRITICAL(&s_snapshot_mux);
}

static void snapshot_set_temperature(float temperature_c) {
    portENTER_CRITICAL(&s_snapshot_mux);
    s_snapshot.temperature_c = temperature_c;
    portEXIT_CRITICAL(&s_snapshot_mux);
}

static void snapshot_set_measurement(bool valid, float voltage_v,
                                     float current_ma) {
    portENTER_CRITICAL(&s_snapshot_mux);
    s_snapshot.voltage_v = valid ? voltage_v : NAN;
    s_snapshot.current_ma = valid ? current_ma : NAN;
    portEXIT_CRITICAL(&s_snapshot_mux);
}

static void update_retained_from_core(void) {
    /* Only durable states are retained.  A reset during STARTING/STOPPING
     * restores IDLE, so the output is driven off on the next boot. */
    s_retained.load.mode = s_core.load.state == LOAD_FSM_WORKING
                               ? LOAD_WORKING
                               : LOAD_IDLE;
    s_retained.load.low_current_pending = s_core.load.low_current_pending;
    s_retained.load.low_current_since_us = s_core.load.low_current_since_us;

    s_retained.buzzer_low_pending = s_core.buzzer.low_pending;
    s_retained.buzzer_low_alerted = s_core.buzzer.low_alerted;
    s_retained.buzzer_low_since_us = s_core.buzzer.low_since_us;
}

static void log_transitions(load_fsm_state_t previous_load,
                            buzzer_fsm_state_t previous_buzzer,
                            ble_fsm_state_t previous_ble) {
    if (previous_load != s_core.load.state) {
        ESP_LOGI(TAG, "load %s -> %s",
                 load_fsm_state_name(previous_load),
                 load_fsm_state_name(s_core.load.state));
    }
    if (previous_buzzer != s_core.buzzer.state) {
        ESP_LOGI(TAG, "buzzer %s -> %s op=%d",
                 buzzer_fsm_state_name(previous_buzzer),
                 buzzer_fsm_state_name(s_core.buzzer.state),
                 (int)s_core.buzzer.operation);
    }
    if (previous_ble != s_core.ble.state) {
        ESP_LOGI(TAG, "ble %s -> %s",
                 ble_fsm_state_name(previous_ble),
                 ble_fsm_state_name(s_core.ble.state));
    }
}

static void process_event_locked(const runtime_event_t *event);

static void apply_commands_locked(const runtime_command_list_t *commands,
                                  int64_t now_us) {
    runtime_event_t follow_up[RUNTIME_COMMAND_MAX];
    uint8_t follow_up_count = 0;

    for (uint8_t i = 0; i < commands->count; ++i) {
        const runtime_command_t *command = &commands->items[i];
        runtime_event_t follow_event;

        if (runtime_core_command_event(command, &follow_event)) {
            follow_event.timestamp_us = now_us;
            if (follow_up_count < RUNTIME_COMMAND_MAX) {
                follow_up[follow_up_count++] = follow_event;
            }
            continue;
        }

        if (command->kind == RCMD_BLE_START &&
            !fsm_is_paired() && !pair_mode_is_active()) {
            if (!pair_mode_activate()) {
                ESP_LOGW(TAG, "pairing mode activation failed");
            }
        }

        if (command->kind == RCMD_LOAD_OUTPUT_ON ||
            command->kind == RCMD_LOAD_OUTPUT_OFF) {
            ESP_LOGI(TAG, "action load_output=%s op=%u",
                     command->bool_value ? "ON" : "OFF",
                     (unsigned)command->operation_id);
        }

        if (!action_executor_submit(command)) {
            ESP_LOGE(TAG, "action submit failed kind=%d op=%u",
                     (int)command->kind, (unsigned)command->operation_id);
            runtime_event_t failed = runtime_event_make(
                RTEVT_ACTION_FAILED, now_us);
            failed.command_kind = (int32_t)command->kind;
            failed.operation_id = command->operation_id;
            failed.request_id = command->request_id;
            failed.context = command->context;
            failed.error = RUNTIME_ERROR_ACTION_REJECTED;
            if (follow_up_count < RUNTIME_COMMAND_MAX) {
                follow_up[follow_up_count++] = failed;
            } else {
                ESP_LOGE(TAG, "action failure event capacity exhausted");
            }
        }
    }

    /* Dispatcher-level commands become events only after all hardware
     * commands for the current event have been submitted. */
    for (uint8_t i = 0; i < follow_up_count; ++i) {
        process_event_locked(&follow_up[i]);
    }
}

static void process_event_locked(const runtime_event_t *event) {
    int64_t now_us = event->timestamp_us > 0 ? event->timestamp_us
                                             : wall_time_us();

    if (event->kind == RTEVT_BLE_PAIR_TIMEOUT) {
        if (!pair_mode_is_active() ||
            event->request_id != pair_mode_deadline_ms()) {
            ESP_LOGI(TAG, "stale pair timeout request=%u deadline=%u",
                     (unsigned)event->request_id,
                     (unsigned)pair_mode_deadline_ms());
            return;
        }
        pair_mode_deactivate();
    }

    if ((event->kind == RTEVT_INA_SAMPLE_OK ||
         event->kind == RTEVT_INA_SAMPLE_FAILED) &&
        event->context == MEASURE_PERIODIC) {
        if (!s_periodic_pending ||
            event->request_id != s_periodic_request_id) {
            ESP_LOGD(TAG, "stale periodic sample request=%u",
                     (unsigned)event->request_id);
            return;
        }
        s_periodic_pending = false;
        s_boot_measurement_pending = false;
        bool valid = event->kind == RTEVT_INA_SAMPLE_OK;
        snapshot_set_measurement(valid, event->voltage_v, event->current_ma);
    } else if (event->kind == RTEVT_INA_SAMPLE_OK ||
               event->kind == RTEVT_INA_SAMPLE_FAILED) {
        /* Only expose a startup/shutdown reading that still belongs to the
         * Buzzer's current measurement request. */
        bool accepted = s_core.buzzer.state == BUZZER_FSM_MEASURING &&
                        event->request_id != 0 &&
                        event->request_id == s_core.buzzer.request_id;
        if (accepted) {
            bool valid = event->kind == RTEVT_INA_SAMPLE_OK;
            snapshot_set_measurement(valid, event->voltage_v,
                                     event->current_ma);
        } else {
            ESP_LOGD(TAG, "stale measurement request=%u context=%d",
                     (unsigned)event->request_id, (int)event->context);
        }
    }

    refresh_configs();

    load_fsm_state_t previous_load = s_core.load.state;
    buzzer_fsm_state_t previous_buzzer = s_core.buzzer.state;
    ble_fsm_state_t previous_ble = s_core.ble.state;

    runtime_command_list_t commands;
    runtime_command_list_init(&commands);
    runtime_core_step(&s_core, event, now_us, &commands);

    log_transitions(previous_load, previous_buzzer, previous_ble);

    if (event->kind == RTEVT_SLEEP_ABORTED) {
        s_sleep_requested = false;
        ESP_LOGI(TAG, "sleep request aborted");
    }
    if (event->kind == RTEVT_ACTION_FAILED &&
        event->command_kind == RCMD_ENTER_DEEP_SLEEP) {
        s_sleep_requested = false;
    }

    /* Pairing mode has no meaning once the radio is fully off. */
    if (s_core.ble.state == BLE_FSM_OFF && pair_mode_is_active()) {
        pair_mode_deactivate();
    }

    update_retained_from_core();
    snapshot_update_load();
    apply_commands_locked(&commands, now_us);
    update_retained_from_core();
    snapshot_update_load();
    load_metrics_publish();
}

static void handle_pair_button_locked(int64_t now_us) {
    bool pending = take_pair_button_press();
    bool level_active =
        gpio_get_level(PAIR_BUTTON_GPIO) == PAIR_BUTTON_ACTIVE_LEVEL;

    if (!pending) {
        if (!level_active) s_retained.reset_button_was_down = false;
        return;
    }

    /* Ignore contact bounce while the same physical press is still held. */
    if (s_retained.reset_button_was_down && level_active) {
        return;
    }

    s_retained.reset_button_was_down = true;
    ESP_LOGI(TAG, "GPIO19 button pressed action=global_reset");

    esp_err_t config_err = runtime_config_reset_defaults();
    if (config_err != ESP_OK) {
        ESP_LOGE(TAG, "global reset: config defaults not saved: %s",
                 esp_err_to_name(config_err));
    }

    s_core.buzzer.low_pending = false;
    s_core.buzzer.low_alerted = false;
    s_core.buzzer.low_since_us = 0;
    s_retained.buzzer_low_pending = false;
    s_retained.buzzer_low_alerted = false;
    s_retained.buzzer_low_since_us = 0;

    trust_reset();
    if (!pair_mode_activate()) {
        ESP_LOGW(TAG, "pairing mode activation failed");
    }

    runtime_event_t event = runtime_event_make(
        RTEVT_BLE_PAIRING_REQUEST, now_us);
    process_event_locked(&event);

    if (gpio_get_level(PAIR_BUTTON_GPIO) != PAIR_BUTTON_ACTIVE_LEVEL) {
        s_retained.reset_button_was_down = false;
    }
}

static void periodic_cycle_locked(int64_t now_us) {
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
    if (ads1115_init() && ads1115_read_raw(BUTTON_ADS_INPUT, &raw) &&
        raw >= 500) {
        temperature = ads1115_raw_to_temp(raw);
    }

    bool pressed = load_state_update_button(&s_retained.load, &policy,
                                            temperature);
    snapshot_set_temperature(temperature);

    esp_err_t led_err = gpio_set_level(
        BUTTON_LED_GPIO, s_retained.load.button_was_pressed ? 1 : 0);
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "button LED set failed: %s", esp_err_to_name(led_err));
    }

    if (pressed) {
        runtime_event_t event = runtime_event_make(
            RTEVT_THERMAL_BUTTON_PRESSED, now_us);
        process_event_locked(&event);
    }

    if (!s_periodic_pending) {
        runtime_command_t request = runtime_command_make(RCMD_INA_REQUEST, 0);
        request.request_id = runtime_event_next_id();
        request.context = MEASURE_PERIODIC;
        s_periodic_pending = true;
        s_periodic_request_id = request.request_id;
        if (!action_executor_submit(&request)) {
            ESP_LOGW(TAG, "periodic INA request not submitted");
            runtime_event_t failed = runtime_event_make(
                RTEVT_INA_SAMPLE_FAILED, now_us);
            failed.request_id = request.request_id;
            failed.context = MEASURE_PERIODIC;
            failed.error = RUNTIME_ERROR_ACTION_REJECTED;
            process_event_locked(&failed);
        }
    }

    load_metrics_publish();
}

static void maybe_request_sleep_locked(int64_t now_us) {
    if (s_sleep_requested) return;

    if (s_boot_measurement_pending) {
        if (now_us - s_boot_started_us < BOOT_MEASUREMENT_TIMEOUT_US) return;
        ESP_LOGW(TAG, "boot measurement timeout; allowing sleep");
        s_boot_measurement_pending = false;
    }

    if (event_bus_pending() != 0) return;

    power_snapshot_t snapshot;
    load_runtime_get_power_snapshot(&snapshot);
    power_sleep_block_reason_t reason =
        power_manager_sleep_block_reason(&snapshot);
    if (reason != POWER_SLEEP_ALLOWED) return;

    runtime_command_t command = runtime_command_make(
        RCMD_ENTER_DEEP_SLEEP, 0);
    if (action_executor_submit(&command)) {
        s_sleep_requested = true;
        ESP_LOGI(TAG, "sleep requested");
    } else {
        ESP_LOGW(TAG, "sleep command not submitted");
    }
}

static void scheduler_locked(int64_t now_us) {
    handle_pair_button_locked(now_us);

    if (now_us >= s_next_periodic_us) {
        s_next_periodic_us = now_us + PERIODIC_INTERVAL_US;
        periodic_cycle_locked(now_us);
    }

    if (now_us >= s_next_deadline_us) {
        s_next_deadline_us = now_us + DEADLINE_CHECK_INTERVAL_US;
        runtime_event_t event = runtime_event_make(RTEVT_DEADLINE_CHECK,
                                                   now_us);
        process_event_locked(&event);
    }

    if (now_us >= s_next_sleep_us) {
        s_next_sleep_us = now_us + SLEEP_CHECK_INTERVAL_US;
        maybe_request_sleep_locked(now_us);
    }
}

void load_runtime_init(void) {
    retained_init();

    bool restored_working = s_retained.load.mode == LOAD_WORKING;
    esp_err_t output_err = load_output_restore(restored_working);
    if (output_err != ESP_OK) {
        ESP_LOGE(TAG, "load output restore failed: %s",
                 esp_err_to_name(output_err));
        (void)load_output_set(false);
        s_retained.load.mode = LOAD_IDLE;
        restored_working = false;
    }

    const gpio_config_t buzzer = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&buzzer));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_0, 0));

    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << BUTTON_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    ESP_ERROR_CHECK(gpio_set_level(
        BUTTON_LED_GPIO, s_retained.load.button_was_pressed ? 1 : 0));

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
    ESP_ERROR_CHECK(gpio_isr_handler_add(PAIR_BUTTON_GPIO, pair_button_isr,
                                         NULL));

    if (gpio_get_level(PAIR_BUTTON_GPIO) != PAIR_BUTTON_ACTIVE_LEVEL) {
        s_retained.reset_button_was_down = false;
    } else if (!s_retained.reset_button_was_down) {
        portENTER_CRITICAL(&s_pair_button_mux);
        s_pair_button_irq_pending = true;
        portEXIT_CRITICAL(&s_pair_button_mux);
    }

    refresh_configs();

    if (!event_bus_init() || !action_executor_init()) {
        ESP_LOGE(TAG, "runtime infrastructure initialization failed");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    runtime_core_init(
        &s_core, &s_load_config, &s_buzzer_config, &s_ble_config,
        restored_working,
        restored_working && s_retained.load.low_current_pending,
        s_retained.load.low_current_since_us,
        s_retained.buzzer_low_pending, s_retained.buzzer_low_alerted,
        s_retained.buzzer_low_since_us);

    portENTER_CRITICAL(&s_snapshot_mux);
    s_snapshot.load_state = restored_working ? LOAD_WORKING : LOAD_IDLE;
    s_snapshot.button_pressed =
        s_retained.load.button_was_pressed ? 1U : 0U;
    s_snapshot.temperature_c = NAN;
    s_snapshot.voltage_v = NAN;
    s_snapshot.current_ma = NAN;
    portEXIT_CRITICAL(&s_snapshot_mux);

    s_boot_measurement_pending = true;
    s_boot_started_us = wall_time_us();
    s_next_periodic_us = 0;
    s_next_deadline_us = 0;
    s_next_sleep_us = 0;

    ESP_LOGI(TAG, "restored load=%s button=%d",
             restored_working ? "WORKING" : "IDLE",
             (int)s_retained.load.button_was_pressed);
}

void load_runtime_task(void *arg) {
    (void)arg;

    for (;;) {
        if (!event_bus_lock_dispatch(DISPATCH_LOCK_TIMEOUT_MS)) {
            /* The action executor is evaluating/entering Deep Sleep.  Hold
             * the event until the dispatcher lock is available again. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (event_bus_delivery_faulted() && !s_event_bus_fault_reported) {
            runtime_event_t fault =
                runtime_event_make(RTEVT_EVENT_BUS_FAULT, wall_time_us());
            process_event_locked(&fault);
            s_event_bus_fault_reported = true;
            ESP_LOGE(TAG, "event bus lost an event; runtime failed closed");
        }

        runtime_event_t pending_event;
        if (event_bus_receive(&pending_event, DISPATCH_RECEIVE_TIMEOUT_MS)) {
            process_event_locked(&pending_event);
        }
        scheduler_locked(wall_time_us());
        event_bus_unlock_dispatch();
    }
}

void load_runtime_get_snapshot(load_runtime_snapshot_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_snapshot_mux);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_mux);
}

void load_runtime_get_power_snapshot(power_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->ble_state = s_core.ble.state;
    out->load_state = s_core.load.state;
    out->buzzer_state = s_core.buzzer.state;
    out->ble_connected = s_core.ble.conn_handle != UINT16_MAX;
    out->ble_radio_quiet = gap_is_radio_quiet();
    out->action_pending = action_executor_pending() > 0;
    out->event_pending = event_bus_pending() > 0;
    out->critical_pending = pair_button_irq_pending() ||
                            s_boot_measurement_pending ||
                            event_bus_delivery_faulted();
}

bool load_runtime_prepare_sleep(void) {
    if (gpio_get_level(PAIR_BUTTON_GPIO) == PAIR_BUTTON_ACTIVE_LEVEL ||
        pair_button_irq_pending()) {
        ESP_LOGI(TAG, "sleep aborted: pair button activity");
        return false;
    }

    if (s_core.ble.state != BLE_FSM_OFF ||
        s_core.ble.conn_handle != UINT16_MAX ||
        s_core.load.state == LOAD_FSM_STARTING ||
        s_core.load.state == LOAD_FSM_STOPPING ||
        s_core.load.state == LOAD_FSM_ERROR ||
        s_core.buzzer.state != BUZZER_FSM_IDLE ||
        event_bus_pending() != 0 || action_executor_pending() != 0 ||
        event_bus_delivery_faulted() || event_bus_sleep_abort_requested()) {
        ESP_LOGW(TAG, "sleep aborted: runtime state changed");
        return false;
    }

    runtime_config_t config;
    runtime_config_get(&config);
    esp_err_t err = esp_sleep_enable_timer_wakeup(
        (uint64_t)config.deep_sleep_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer wakeup config failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = esp_deep_sleep_enable_gpio_wakeup(
        1ULL << PAIR_BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO wakeup config failed: %s", esp_err_to_name(err));
    }

    err = load_output_hold();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load output hold failed: %s", esp_err_to_name(err));
        return false;
    }

    (void)gpio_set_level(GPIO_NUM_0, 0);
    if (event_bus_pending() != 0 || event_bus_delivery_faulted() ||
        event_bus_sleep_abort_requested() || pair_button_irq_pending() ||
        gpio_get_level(PAIR_BUTTON_GPIO) == PAIR_BUTTON_ACTIVE_LEVEL) {
        ESP_LOGW(TAG, "sleep aborted: event arrived during final preparation");
        return false;
    }
    ESP_LOGI(TAG, "entering deep sleep for %u ms",
             (unsigned)config.deep_sleep_ms);
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "deep sleep returned unexpectedly");
    return false;
}
