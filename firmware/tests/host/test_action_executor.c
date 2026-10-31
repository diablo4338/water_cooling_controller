#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/queue.h"

#include "../../src/ble_protocol/app/action_executor.c"

struct host_queue {
    size_t capacity;
    size_t item_size;
    size_t count;
    size_t head;
    size_t tail;
    unsigned char *items;
};

struct host_timer {
    esp_timer_cb_t callback;
    void *arg;
    bool active;
    int64_t deadline_us;
};

static struct host_timer timers[3];
static size_t timer_count;
static int64_t now_us;
static int fail_next_timer_start;
static int fail_next_gpio_set;
static int fail_next_gpio_get;
static int gpio_levels[32];
static runtime_event_t events[32];
static size_t event_count;
static bool event_publish_result = true;
static size_t sleep_attempt_finished_count;
static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    struct host_queue *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    queue->items = calloc(length, item_size);
    if (!queue->items) {
        free(queue);
        return NULL;
    }
    queue->capacity = length;
    queue->item_size = item_size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t wait) {
    (void)wait;
    struct host_queue *queue = handle;
    if (!queue || queue->count == queue->capacity) return pdFALSE;
    memcpy(queue->items + queue->tail * queue->item_size, item,
           queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->capacity;
    ++queue->count;
    return pdTRUE;
}

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *higher_priority_task_woken) {
    if (higher_priority_task_woken) *higher_priority_task_woken = pdFALSE;
    return xQueueSend(queue, item, 0);
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t wait) {
    (void)wait;
    struct host_queue *queue = handle;
    if (!queue || queue->count == 0U) return pdFALSE;
    memcpy(item, queue->items + queue->head * queue->item_size,
           queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t handle) {
    struct host_queue *queue = handle;
    return queue ? (UBaseType_t)queue->count : 0U;
}

void vTaskDelay(TickType_t ticks) {
    (void)ticks;
}

BaseType_t xTaskCreate(void (*task)(void *), const char *name,
                       uint32_t stack_depth, void *arg, UBaseType_t priority,
                       void *task_handle) {
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)arg;
    (void)priority;
    (void)task_handle;
    return pdPASS;
}

esp_timer_handle_t unused_timer;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *timer) {
    if (timer_count >= sizeof(timers) / sizeof(timers[0])) return ESP_FAIL;
    struct host_timer *created = &timers[timer_count++];
    created->callback = args->callback;
    created->arg = args->arg;
    *timer = created;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    if (fail_next_timer_start) {
        --fail_next_timer_start;
        return ESP_FAIL;
    }
    timer->active = true;
    timer->deadline_us = now_us + (int64_t)timeout_us;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer) timer->active = false;
    return ESP_OK;
}

int64_t esp_timer_get_time(void) {
    return now_us;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, int level) {
    if (fail_next_gpio_set) {
        --fail_next_gpio_set;
        return ESP_FAIL;
    }
    if (gpio_num < 0 || gpio_num >= 32) return ESP_ERR_INVALID_ARG;
    gpio_levels[gpio_num] = level;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num) {
    if (fail_next_gpio_get) {
        --fail_next_gpio_get;
        return -1;
    }
    if (gpio_num < 0 || gpio_num >= 32) return -1;
    return gpio_levels[gpio_num];
}
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
void gpio_deep_sleep_hold_dis(void) {}
esp_err_t gpio_hold_dis(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
esp_err_t gpio_hold_en(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
void gpio_deep_sleep_hold_en(void) {}

const char *esp_err_to_name(esp_err_t error) {
    (void)error;
    return "host error";
}

bool event_bus_publish(const runtime_event_t *event) {
    if (event_count < sizeof(events) / sizeof(events[0])) {
        events[event_count++] = *event;
    }
    return event_publish_result;
}

bool event_bus_publish_isr(const runtime_event_t *event) {
    return event_bus_publish(event);
}

bool event_bus_lock_dispatch(uint32_t timeout_ms) {
    (void)timeout_ms;
    return true;
}

void event_bus_unlock_dispatch(void) {}
bool event_bus_begin_sleep_gate(void) { return true; }
void event_bus_cancel_sleep_gate(void) {}
bool event_bus_sleep_abort_requested(void) { return false; }
size_t event_bus_pending(void) { return 0; }
void load_runtime_get_power_snapshot(power_snapshot_t *out) { memset(out, 0, sizeof(*out)); }
bool load_runtime_prepare_sleep(void) { return false; }
void load_runtime_sleep_attempt_finished(void) {
    ++sleep_attempt_finished_count;
}
bool ina226_get_single_sample_ex(ina226_sample_t *sample, esp_err_t *error) {
    (void)sample;
    *error = ESP_FAIL;
    return false;
}
bool gap_ble_start(uint32_t operation_id) { (void)operation_id; return false; }
bool gap_start_advertising(uint32_t operation_id, uint32_t request_id,
                           uint32_t duration_ms) {
    (void)operation_id; (void)request_id; (void)duration_ms; return false;
}
bool gap_refresh_advertising(uint32_t operation_id, uint32_t request_id,
                             uint32_t duration_ms) {
    (void)operation_id; (void)request_id; (void)duration_ms; return false;
}
void gap_disconnect_client(void) {}
const char *power_manager_block_reason_name(power_sleep_block_reason_t reason) {
    (void)reason;
    return "host";
}
power_sleep_block_reason_t power_manager_sleep_block_reason(
    const power_snapshot_t *snapshot) {
    (void)snapshot;
    return POWER_BLOCK_ACTION;
}

static void reset_events(void) {
    event_count = 0;
    memset(events, 0, sizeof(events));
}

static void execute_next(void) {
    runtime_command_t command;
    CHECK(xQueueReceive(s_queue, &command, 0) == pdTRUE);
    execute_command(&command);
}

static void submit_and_execute(runtime_command_t command) {
    CHECK(action_executor_submit(&command));
    execute_next();
}

static size_t count_events(runtime_event_kind_t kind, uint32_t operation_id) {
    size_t count = 0;
    for (size_t i = 0; i < event_count; ++i) {
        if (events[i].kind == kind &&
            events[i].operation_id == operation_id) ++count;
    }
    return count;
}

int main(void) {
    CHECK(action_executor_init());
    runtime_command_t play = runtime_command_make(RCMD_BUZZER_PLAY, 1);
    play.duration_ms = 80;
    play.off_ms = 40;
    play.repeats = 2;

    reset_events();
    submit_and_execute(play);
    CHECK(gpio_levels[GPIO_NUM_0] == 1);
    now_us = 80000;
    beep_timer_cb(NULL);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(count_events(RTEVT_BEEP_FINISHED, 1) == 0);
    now_us = 120000;
    beep_timer_cb(NULL);
    CHECK(gpio_levels[GPIO_NUM_0] == 1);
    now_us = 200000;
    beep_timer_cb(NULL);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(count_events(RTEVT_BEEP_FINISHED, 1) == 1);
    beep_timer_cb(NULL);
    CHECK(count_events(RTEVT_BEEP_FINISHED, 1) == 1);

    reset_events();
    play.operation_id = 2;
    submit_and_execute(play);
    now_us += 1000;
    beep_timer_cb(NULL);
    CHECK(gpio_levels[GPIO_NUM_0] == 1);
    CHECK(count_events(RTEVT_BEEP_FINISHED, 2) == 0);

    play.operation_id = 3;
    submit_and_execute(play);
    now_us += 80000;
    beep_timer_cb(NULL);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(s_beep.active);
    runtime_command_t stale_cancel = runtime_command_make(RCMD_BUZZER_CANCEL, 2);
    submit_and_execute(stale_cancel);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(s_beep.active);

    runtime_command_t cancel = runtime_command_make(RCMD_BUZZER_CANCEL, 3);
    submit_and_execute(cancel);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(!s_beep.active);
    now_us += 100000;
    beep_timer_cb(NULL);
    CHECK(count_events(RTEVT_BEEP_FINISHED, 3) == 0);

    play.operation_id = 4;
    submit_and_execute(play);
    now_us += 1000;
    beep_timer_cb(NULL);
    CHECK(gpio_levels[GPIO_NUM_0] == 1);
    CHECK(s_beep.active);
    cancel.operation_id = 4;
    submit_and_execute(cancel);

    reset_events();
    play.operation_id = 5;
    fail_next_timer_start = 1;
    submit_and_execute(play);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(!s_beep.active);
    CHECK(count_events(RTEVT_ACTION_FAILED, 5) == 1);

    reset_events();
    play.operation_id = 6;
    fail_next_gpio_set = 1;
    submit_and_execute(play);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(count_events(RTEVT_ACTION_FAILED, 6) == 1);

    reset_events();
    runtime_command_t load_off = runtime_command_make(RCMD_LOAD_OUTPUT_OFF, 7);
    load_off.bool_value = false;
    fail_next_gpio_set = 1;
    submit_and_execute(load_off);
    CHECK(count_events(RTEVT_LOAD_GPIO_FAILED, 7) == 1);
    reset_events();
    runtime_command_t load_on = runtime_command_make(RCMD_LOAD_OUTPUT_ON, 8);
    load_on.bool_value = true;
    submit_and_execute(load_on);
    CHECK(count_events(RTEVT_LOAD_GPIO_APPLIED, 8) == 1);
    CHECK(gpio_levels[GPIO_NUM_3] == 1);
    CHECK(gpio_levels[GPIO_NUM_10] == 0);

    reset_events();
    event_publish_result = false;
    load_off.operation_id = 14;
    submit_and_execute(load_off);
    event_publish_result = true;
    CHECK(gpio_levels[GPIO_NUM_3] == 0);
    CHECK(gpio_levels[GPIO_NUM_10] == 1);
    runtime_command_t verify_off =
        runtime_command_make(RCMD_LOAD_OUTPUT_VERIFY, 14);
    verify_off.bool_value = false;
    submit_and_execute(verify_off);
    CHECK(count_events(RTEVT_LOAD_GPIO_VERIFIED, 14) == 1);
    CHECK(events[event_count - 1].command_kind == RCMD_LOAD_OUTPUT_OFF);
    CHECK(events[event_count - 1].bool_value);
    CHECK(events[event_count - 1].error == ESP_OK);

    reset_events();
    event_publish_result = false;
    load_on.operation_id = 15;
    submit_and_execute(load_on);
    event_publish_result = true;
    CHECK(gpio_levels[GPIO_NUM_3] == 1);
    CHECK(gpio_levels[GPIO_NUM_10] == 0);
    runtime_command_t verify_on =
        runtime_command_make(RCMD_LOAD_OUTPUT_VERIFY, 15);
    verify_on.bool_value = true;
    submit_and_execute(verify_on);
    CHECK(count_events(RTEVT_LOAD_GPIO_VERIFIED, 15) == 1);
    CHECK(events[event_count - 1].command_kind == RCMD_LOAD_OUTPUT_ON);
    CHECK(events[event_count - 1].bool_value);
    CHECK(events[event_count - 1].error == ESP_OK);

    reset_events();
    gpio_levels[GPIO_NUM_10] = 1;
    verify_on.operation_id = 16;
    submit_and_execute(verify_on);
    CHECK(!events[event_count - 1].bool_value);
    CHECK(events[event_count - 1].error == ESP_OK);
    CHECK(gpio_levels[GPIO_NUM_10] == 1);

    reset_events();
    fail_next_gpio_get = 1;
    verify_on.operation_id = 17;
    submit_and_execute(verify_on);
    CHECK(!events[event_count - 1].bool_value);
    CHECK(events[event_count - 1].error != ESP_OK);

    reset_events();
    play.operation_id = 9;
    play.duration_ms = 10;
    play.repeats = 1;
    submit_and_execute(play);
    now_us += 10000;
    fail_next_gpio_set = 1;
    beep_timer_cb(NULL);
    CHECK(gpio_levels[GPIO_NUM_0] == 0);
    CHECK(count_events(RTEVT_ACTION_FAILED, 9) == 1);
    CHECK(count_events(RTEVT_BEEP_FINISHED, 9) == 0);

    reset_events();
    play.operation_id = 10;
    play.duration_ms = 80;
    play.repeats = 2;
    submit_and_execute(play);
    fail_next_gpio_set = 1;
    cancel.operation_id = 10;
    submit_and_execute(cancel);
    CHECK(count_events(RTEVT_ACTION_FAILED, 10) == 1);
    CHECK(!s_beep.active);
    CHECK(gpio_levels[GPIO_NUM_0] == 1);
    CHECK(gpio_set_level(GPIO_NUM_0, 0) == ESP_OK);

    play.operation_id = 11;
    submit_and_execute(play);
    runtime_command_t queued = runtime_command_make(RCMD_NONE, 0);
    for (size_t i = 0; i < ACTION_QUEUE_DEPTH; ++i) {
        CHECK(action_executor_submit(&queued));
    }
    cancel.operation_id = 11;
    CHECK(!action_executor_submit(&cancel));
    CHECK(s_beep.active);
    for (size_t i = 0; i < ACTION_QUEUE_DEPTH; ++i) execute_next();
    submit_and_execute(cancel);
    CHECK(!s_beep.active);

    reset_events();
    event_publish_result = false;
    runtime_command_t sleep = runtime_command_make(RCMD_ENTER_DEEP_SLEEP, 0);
    size_t sleep_finished_before = sleep_attempt_finished_count;
    execute_command(&sleep);
    CHECK(sleep_attempt_finished_count == sleep_finished_before + 1U);
    event_publish_result = true;

    if (failures != 0) {
        fprintf(stderr, "%d action-executor assertions failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("action executor tests passed");
    return EXIT_SUCCESS;
}
