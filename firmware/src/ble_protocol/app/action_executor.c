#include "action_executor.h"

#include <sys/time.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "event_bus.h"
#include "gap.h"
#include "ina226.h"
#include "load_output.h"
#include "load_runtime.h"
#include "power_manager.h"

#define ACTION_QUEUE_DEPTH 32U
#define ACTION_SUBMIT_WAIT_MS 50U
#define BUZZER_GPIO GPIO_NUM_0

static const char *TAG = "action";
static QueueHandle_t s_queue;
static volatile bool s_busy;
static volatile bool s_executing_sleep;

static esp_timer_handle_t s_settle_timer;
static esp_timer_handle_t s_measure_timer;
static esp_timer_handle_t s_beep_timer;

static volatile uint32_t s_settle_operation_id;
static volatile uint32_t s_measure_request_id;
static volatile uint32_t s_measure_operation_id;
static volatile measure_context_t s_measure_context;

static portMUX_TYPE s_beep_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool active;
    bool phase_on;
    uint8_t repeats_left;
    uint32_t operation_id;
    uint16_t on_ms;
    uint16_t off_ms;
    uint32_t generation;
    int64_t phase_deadline_us;
} s_beep;

static int64_t wall_time_us(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_usec;
}

static void publish_simple(runtime_event_kind_t kind, uint32_t operation_id) {
    runtime_event_t event = runtime_event_make(kind, wall_time_us());
    event.operation_id = operation_id;
    event_bus_publish(&event);
}

static void publish_action_failure(runtime_command_kind_t command_kind,
                                   uint32_t operation_id,
                                   uint32_t request_id,
                                   measure_context_t context,
                                   int32_t error) {
    runtime_event_t event = runtime_event_make(RTEVT_ACTION_FAILED,
                                               wall_time_us());
    event.command_kind = (int32_t)command_kind;
    event.operation_id = operation_id;
    event.request_id = request_id;
    event.context = context;
    event.error = error;
    event_bus_publish(&event);
}

static void settle_timer_cb(void *arg) {
    (void)arg;
    runtime_event_t event = runtime_event_make(RTEVT_SETTLE_ELAPSED,
                                               wall_time_us());
    event.operation_id = s_settle_operation_id;
    event_bus_publish(&event);
}

static void measure_timer_cb(void *arg) {
    (void)arg;
    runtime_event_t event = runtime_event_make(RTEVT_MEASURE_TIMEOUT,
                                               wall_time_us());
    event.request_id = s_measure_request_id;
    event.operation_id = s_measure_operation_id;
    event.context = s_measure_context;
    event_bus_publish(&event);
}

static void beep_finish(uint32_t operation_id) {
    runtime_event_t event = runtime_event_make(RTEVT_BEEP_FINISHED,
                                               wall_time_us());
    event.operation_id = operation_id;
    event_bus_publish(&event);
}

static void beep_fail(runtime_command_kind_t command_kind,
                      uint32_t operation_id, esp_err_t error) {
    publish_action_failure(command_kind, operation_id, 0, MEASURE_PERIODIC,
                           error);
}

static void beep_timer_cb(void *arg);

static bool beep_arm(uint32_t generation, uint32_t delay_ms) {
    if (delay_ms == 0) delay_ms = 1;
    portENTER_CRITICAL(&s_beep_mux);
    bool current = s_beep.active && s_beep.generation == generation;
    if (current) {
        s_beep.phase_deadline_us =
            esp_timer_get_time() + (int64_t)delay_ms * 1000LL;
    }
    portEXIT_CRITICAL(&s_beep_mux);
    if (!current) return false;
    return esp_timer_start_once(s_beep_timer, (uint64_t)delay_ms * 1000ULL) ==
           ESP_OK;
}

static void beep_timer_cb(void *arg) {
    (void)arg;
    bool done = false;
    bool drive_high = false;
    uint32_t operation_id = 0;
    uint32_t generation = 0;
    uint32_t next_ms = 0;
    esp_err_t cleanup_err = ESP_OK;

    /* A delayed callback from an old phase must not advance a newer phase. */
    portENTER_CRITICAL(&s_beep_mux);
    if (!s_beep.active) {
        portEXIT_CRITICAL(&s_beep_mux);
        return;
    }
    if (esp_timer_get_time() < s_beep.phase_deadline_us) {
        portEXIT_CRITICAL(&s_beep_mux);
        return;
    }
    generation = s_beep.generation;
    operation_id = s_beep.operation_id;
    if (s_beep.phase_on) {
        s_beep.phase_on = false;
        if (s_beep.repeats_left > 0) --s_beep.repeats_left;
        if (s_beep.repeats_left > 0) {
            next_ms = s_beep.off_ms;
        } else {
            s_beep.active = false;
            done = true;
        }
    } else {
        s_beep.phase_on = true;
        drive_high = true;
        next_ms = s_beep.on_ms;
    }
    esp_err_t gpio_err = gpio_set_level(BUZZER_GPIO, drive_high ? 1 : 0);
    if (gpio_err != ESP_OK) {
        s_beep.active = false;
        ++s_beep.generation;
        s_beep.phase_deadline_us = 0;
        (void)gpio_set_level(BUZZER_GPIO, 0);
    }
    portEXIT_CRITICAL(&s_beep_mux);

    if (gpio_err != ESP_OK) {
        esp_timer_stop(s_beep_timer);
        beep_fail(RCMD_BUZZER_PLAY, operation_id, gpio_err);
        return;
    }

    if (done) {
        beep_finish(operation_id);
    } else if (!beep_arm(generation, next_ms)) {
        bool current;
        portENTER_CRITICAL(&s_beep_mux);
        current = s_beep.active && s_beep.generation == generation;
        if (current) {
            s_beep.active = false;
            ++s_beep.generation;
            s_beep.phase_deadline_us = 0;
            cleanup_err = gpio_set_level(BUZZER_GPIO, 0);
        }
        portEXIT_CRITICAL(&s_beep_mux);
        if (current) {
            beep_fail(RCMD_BUZZER_PLAY, operation_id,
                      cleanup_err == ESP_OK ? ESP_FAIL : cleanup_err);
        }
    }
}

static void beep_start(const runtime_command_t *command) {
    esp_timer_stop(s_beep_timer);

    portENTER_CRITICAL(&s_beep_mux);
    ++s_beep.generation;
    uint32_t generation = s_beep.generation;
    s_beep.operation_id = command->operation_id;
    s_beep.on_ms = (uint16_t)command->duration_ms;
    s_beep.off_ms = command->off_ms;
    s_beep.repeats_left = command->repeats;
    s_beep.phase_on = true;
    s_beep.active = command->repeats > 0 && command->duration_ms > 0;
    bool active = s_beep.active;
    uint32_t on_ms = s_beep.on_ms;
    s_beep.phase_deadline_us = 0;
    esp_err_t gpio_err = gpio_set_level(BUZZER_GPIO, active ? 1 : 0);
    if (gpio_err != ESP_OK) {
        s_beep.active = false;
        ++s_beep.generation;
        (void)gpio_set_level(BUZZER_GPIO, 0);
    }
    portEXIT_CRITICAL(&s_beep_mux);

    if (gpio_err != ESP_OK) {
        esp_timer_stop(s_beep_timer);
        beep_fail(RCMD_BUZZER_PLAY, command->operation_id, gpio_err);
        return;
    }
    if (!active) {
        beep_finish(command->operation_id);
        return;
    }
    if (!beep_arm(generation, on_ms)) {
        bool current;
        esp_err_t cleanup_err = ESP_OK;
        portENTER_CRITICAL(&s_beep_mux);
        current = s_beep.generation == generation;
        if (current) {
            s_beep.active = false;
            ++s_beep.generation;
            s_beep.phase_deadline_us = 0;
            cleanup_err = gpio_set_level(BUZZER_GPIO, 0);
        }
        portEXIT_CRITICAL(&s_beep_mux);
        if (current) beep_fail(RCMD_BUZZER_PLAY, command->operation_id,
                               cleanup_err == ESP_OK ? ESP_FAIL : cleanup_err);
    }
}

static void beep_cancel(uint32_t operation_id) {
    bool cancelled = false;
    esp_err_t gpio_err = ESP_OK;
    portENTER_CRITICAL(&s_beep_mux);
    if (s_beep.active && s_beep.operation_id == operation_id) {
        ++s_beep.generation;
        s_beep.active = false;
        s_beep.phase_deadline_us = 0;
        gpio_err = gpio_set_level(BUZZER_GPIO, 0);
        cancelled = true;
    }
    portEXIT_CRITICAL(&s_beep_mux);
    if (!cancelled) return;
    esp_timer_stop(s_beep_timer);
    if (gpio_err != ESP_OK) {
        beep_fail(RCMD_BUZZER_CANCEL, operation_id, gpio_err);
    }
}

static void execute_load_output(const runtime_command_t *command) {
    esp_err_t err = load_output_set(command->bool_value);
    runtime_event_t event = runtime_event_make(
        err == ESP_OK ? RTEVT_LOAD_GPIO_APPLIED : RTEVT_LOAD_GPIO_FAILED,
        wall_time_us());
    event.operation_id = command->operation_id;
    event.bool_value = command->bool_value;
    event.error = (int32_t)err;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load output %s failed: %s",
                 command->bool_value ? "ON" : "OFF", esp_err_to_name(err));
    }
    event_bus_publish(&event);
}

static void execute_load_output_verify(const runtime_command_t *command) {
    bool matches = false;
    esp_err_t err = load_output_verify(command->bool_value, &matches);
    runtime_event_t event = runtime_event_make(RTEVT_LOAD_GPIO_VERIFIED,
                                                wall_time_us());
    event.operation_id = command->operation_id;
    event.command_kind = command->bool_value ? RCMD_LOAD_OUTPUT_ON
                                             : RCMD_LOAD_OUTPUT_OFF;
    event.bool_value = err == ESP_OK && matches;
    event.error = (int32_t)err;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load output level read failed: %s", esp_err_to_name(err));
    }
    event_bus_publish(&event);
}

static void execute_ina_request(const runtime_command_t *command) {
    ina226_sample_t sample;
    esp_err_t err = ESP_OK;
    bool ok = ina226_get_single_sample_ex(&sample, &err);
    if (command->context != MEASURE_PERIODIC) {
        /* The correlated result supersedes the operation timeout. */
        esp_timer_stop(s_measure_timer);
    }
    runtime_event_t event = runtime_event_make(
        ok ? RTEVT_INA_SAMPLE_OK : RTEVT_INA_SAMPLE_FAILED, wall_time_us());
    event.operation_id = command->operation_id;
    event.request_id = command->request_id;
    event.context = command->context;
    event.error = (int32_t)err;
    if (ok) {
        event.voltage_v = sample.voltage_v;
        event.current_ma = sample.current_ma;
    }
    event_bus_publish(&event);
}

static void execute_ble_command(const runtime_command_t *command) {
    switch (command->kind) {
        case RCMD_BLE_START:
            if (!gap_ble_start(command->operation_id)) {
                publish_simple(RTEVT_BLE_START_FAILED, command->operation_id);
            }
            break;
        case RCMD_BLE_START_ADVERTISING:
            if (!gap_start_advertising(command->operation_id,
                                       command->request_id,
                                       command->duration_ms)) {
                publish_simple(RTEVT_BLE_START_FAILED, command->operation_id);
            }
            break;
        case RCMD_BLE_REFRESH_ADVERTISING:
            if (!gap_refresh_advertising(command->operation_id,
                                         command->request_id,
                                         command->duration_ms)) {
                publish_simple(RTEVT_BLE_START_FAILED, command->operation_id);
            }
            break;
        case RCMD_BLE_DISCONNECT:
            gap_disconnect_client();
            break;
        default:
            break;
    }
}

static void execute_sleep(void) {
    s_executing_sleep = true;

    /* Keep the dispatcher from mutating FSM state or submitting commands
     * while the final gate is evaluated.  Producers may still publish, but
     * the armed gate rejects them and requests an abort. */
    if (!event_bus_lock_dispatch(100)) {
        s_executing_sleep = false;
        publish_simple(RTEVT_SLEEP_ABORTED, 0);
        load_runtime_sleep_attempt_finished();
        return;
    }
    if (!event_bus_begin_sleep_gate()) {
        event_bus_unlock_dispatch();
        s_executing_sleep = false;
        publish_simple(RTEVT_SLEEP_ABORTED, 0);
        load_runtime_sleep_attempt_finished();
        return;
    }

    power_snapshot_t snapshot;
    load_runtime_get_power_snapshot(&snapshot);
    bool gate_ok = !event_bus_sleep_abort_requested() &&
                   event_bus_pending() == 0 &&
                   action_executor_pending() == 0;
    power_sleep_block_reason_t reason =
        power_manager_sleep_block_reason(&snapshot);
    bool can_sleep = gate_ok && reason == POWER_SLEEP_ALLOWED;

    if (can_sleep && load_runtime_prepare_sleep()) {
        /* Deep sleep entered; this should not return. */
        ESP_LOGE(TAG, "deep sleep returned unexpectedly");
    }

    event_bus_cancel_sleep_gate();
    event_bus_unlock_dispatch();
    s_executing_sleep = false;
    ESP_LOGI(TAG, "sleep aborted reason=%s gate_ok=%d",
             power_manager_block_reason_name(reason), (int)gate_ok);
    publish_simple(RTEVT_SLEEP_ABORTED, 0);
    load_runtime_sleep_attempt_finished();
}

static void execute_command(const runtime_command_t *command) {
    switch (command->kind) {
        case RCMD_LOAD_OUTPUT_ON:
        case RCMD_LOAD_OUTPUT_OFF:
            execute_load_output(command);
            break;
        case RCMD_LOAD_OUTPUT_VERIFY:
            execute_load_output_verify(command);
            break;

        case RCMD_START_SETTLE_TIMER:
            s_settle_operation_id = command->operation_id;
            esp_timer_stop(s_settle_timer);
            {
                esp_err_t err = esp_timer_start_once(
                    s_settle_timer,
                    (uint64_t)command->duration_ms * 1000ULL);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "settle timer start failed: %s",
                             esp_err_to_name(err));
                    publish_action_failure(RCMD_START_SETTLE_TIMER,
                                           command->operation_id, 0,
                                           MEASURE_STARTUP, err);
                }
            }
            break;
        case RCMD_CANCEL_SETTLE_TIMER:
            esp_timer_stop(s_settle_timer);
            break;

        case RCMD_INA_REQUEST:
            execute_ina_request(command);
            break;

        case RCMD_START_MEASURE_TIMEOUT:
            s_measure_request_id = command->request_id;
            s_measure_operation_id = command->operation_id;
            s_measure_context = command->context;
            esp_timer_stop(s_measure_timer);
            {
                esp_err_t err = esp_timer_start_once(
                    s_measure_timer,
                    (uint64_t)command->duration_ms * 1000ULL);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "measure timeout start failed: %s",
                             esp_err_to_name(err));
                    publish_action_failure(RCMD_START_MEASURE_TIMEOUT,
                                           command->operation_id,
                                           command->request_id,
                                           command->context, err);
                }
            }
            break;
        case RCMD_CANCEL_MEASURE_TIMEOUT:
            esp_timer_stop(s_measure_timer);
            break;

        case RCMD_BUZZER_PLAY:
            beep_start(command);
            break;
        case RCMD_BUZZER_CANCEL:
            beep_cancel(command->operation_id);
            break;

        case RCMD_BLE_START:
        case RCMD_BLE_START_ADVERTISING:
        case RCMD_BLE_REFRESH_ADVERTISING:
        case RCMD_BLE_DISCONNECT:
            execute_ble_command(command);
            break;

        case RCMD_ENTER_DEEP_SLEEP:
            execute_sleep();
            break;

        case RCMD_BUZZER_REQUEST_SHUTDOWN:
        case RCMD_EMIT_BUZZER_DONE:
            /* These are dispatcher-level events, never hardware actions. */
            break;

        default:
            break;
    }
}

static void action_executor_task(void *arg) {
    (void)arg;
    for (;;) {
        runtime_command_t command;
        if (xQueueReceive(s_queue, &command, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (command.kind == RCMD_ENTER_DEEP_SLEEP) {
                s_executing_sleep = true;
            } else {
                s_busy = true;
            }
            execute_command(&command);
            s_busy = false;
            s_executing_sleep = false;
        }
    }
}

static esp_timer_handle_t create_timer(esp_timer_cb_t callback,
                                       const char *name) {
    esp_timer_create_args_t args = {
        .callback = callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = name,
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) != ESP_OK) return NULL;
    return timer;
}

bool action_executor_init(void) {
    if (s_queue) return true;
    s_queue = xQueueCreate(ACTION_QUEUE_DEPTH, sizeof(runtime_command_t));
    if (!s_queue) return false;

    s_settle_timer = create_timer(settle_timer_cb, "load_settle");
    s_measure_timer = create_timer(measure_timer_cb, "ina_timeout");
    s_beep_timer = create_timer(beep_timer_cb, "buzzer");
    if (!s_settle_timer || !s_measure_timer || !s_beep_timer) {
        ESP_LOGE(TAG, "timer allocation failed");
        return false;
    }
    gpio_set_level(BUZZER_GPIO, 0);
    if (xTaskCreate(action_executor_task, "actions", 4096, NULL, 4, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return false;
    }
    return true;
}

bool action_executor_submit(const runtime_command_t *command) {
    if (!command || !s_queue) return false;
    if (xQueueSend(s_queue, command,
                   pdMS_TO_TICKS(ACTION_SUBMIT_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "command queue full kind=%d", (int)command->kind);
        return false;
    }
    return true;
}

size_t action_executor_pending(void) {
    if (!s_queue) return 0;
    size_t queued = (size_t)uxQueueMessagesWaiting(s_queue);
    if (s_busy && !s_executing_sleep) ++queued;
    return queued;
}
