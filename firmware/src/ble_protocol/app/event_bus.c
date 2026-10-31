#include "event_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define EVENT_BUS_OVERFLOW_DEPTH 8U

static const char *TAG = "event-bus";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_publish_mutex;
static SemaphoreHandle_t s_dispatch_mutex;
static portMUX_TYPE s_gate_mux = portMUX_INITIALIZER_UNLOCKED;
static runtime_event_t s_overflow_events[EVENT_BUS_OVERFLOW_DEPTH];
static size_t s_overflow_head;
static size_t s_overflow_tail;
static size_t s_overflow_pending;
static volatile bool s_sleep_gate;
static volatile bool s_sleep_abort;
static volatile bool s_delivery_fault;
static volatile uint32_t s_isr_publishers;
static uint32_t s_overflow_count;

static bool overflow_push_locked(const runtime_event_t *event) {
    if (s_overflow_pending == EVENT_BUS_OVERFLOW_DEPTH) {
        s_delivery_fault = true;
        return false;
    }
    s_overflow_events[s_overflow_tail] = *event;
    s_overflow_tail = (s_overflow_tail + 1U) % EVENT_BUS_OVERFLOW_DEPTH;
    ++s_overflow_pending;
    return true;
}

static bool overflow_pop(runtime_event_t *event) {
    bool found = false;
    portENTER_CRITICAL(&s_gate_mux);
    if (s_overflow_pending > 0U) {
        *event = s_overflow_events[s_overflow_head];
        s_overflow_head = (s_overflow_head + 1U) % EVENT_BUS_OVERFLOW_DEPTH;
        --s_overflow_pending;
        found = true;
    }
    portEXIT_CRITICAL(&s_gate_mux);
    return found;
}

bool event_bus_init(void) {
    if (s_queue) return true;
    s_queue = xQueueCreate(EVENT_BUS_DEPTH, sizeof(runtime_event_t));
    s_publish_mutex = xSemaphoreCreateMutex();
    s_dispatch_mutex = xSemaphoreCreateMutex();
    if (!s_queue || !s_publish_mutex || !s_dispatch_mutex) {
        ESP_LOGE(TAG, "queue/mutex allocation failed");
        return false;
    }
    return true;
}

bool event_bus_lock_dispatch(uint32_t timeout_ms) {
    if (!s_dispatch_mutex) return false;
    return xSemaphoreTake(s_dispatch_mutex,
                          pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void event_bus_unlock_dispatch(void) {
    if (s_dispatch_mutex) xSemaphoreGive(s_dispatch_mutex);
}

bool event_bus_publish(const runtime_event_t *event) {
    if (!event || !s_queue || !s_publish_mutex) return false;
    if (xSemaphoreTake(s_publish_mutex,
                       pdMS_TO_TICKS(EVENT_BUS_PUBLISH_WAIT_MS)) != pdTRUE) {
        portENTER_CRITICAL(&s_gate_mux);
        ++s_overflow_count;
        s_delivery_fault = true;
        if (s_sleep_gate) s_sleep_abort = true;
        portEXIT_CRITICAL(&s_gate_mux);
        return false;
    }

    bool gate;
    portENTER_CRITICAL(&s_gate_mux);
    gate = s_sleep_gate;
    if (gate) s_sleep_abort = true;
    portEXIT_CRITICAL(&s_gate_mux);

    bool sent = xQueueSend(s_queue, event, 0) == pdTRUE;
    if (!sent) {
        portENTER_CRITICAL(&s_gate_mux);
        ++s_overflow_count;
        sent = overflow_push_locked(event);
        if (!sent && gate) s_sleep_abort = true;
        portEXIT_CRITICAL(&s_gate_mux);
    }
    xSemaphoreGive(s_publish_mutex);
    if (!sent) {
        ESP_LOGW(TAG, "event %d rejected (gate=%d)", (int)event->kind, (int)gate);
    }
    return sent;
}

bool event_bus_publish_isr(const runtime_event_t *event) {
    if (!event || !s_queue) return false;
    portENTER_CRITICAL_ISR(&s_gate_mux);
    ++s_isr_publishers;
    if (s_sleep_gate) s_sleep_abort = true;
    portEXIT_CRITICAL_ISR(&s_gate_mux);

    BaseType_t woken = pdFALSE;
    bool sent = xQueueSendFromISR(s_queue, event, &woken) == pdTRUE;
    portENTER_CRITICAL_ISR(&s_gate_mux);
    if (!sent) {
        ++s_overflow_count;
        sent = overflow_push_locked(event);
    }
    --s_isr_publishers;
    portEXIT_CRITICAL_ISR(&s_gate_mux);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
    return sent;
}

bool event_bus_receive(runtime_event_t *event, uint32_t timeout_ms) {
    if (!event || !s_queue) return false;
    if (xQueueReceive(s_queue, event, 0) == pdTRUE) return true;
    if (overflow_pop(event)) return true;
    if (xQueueReceive(s_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return true;
    }
    return overflow_pop(event);
}

size_t event_bus_pending(void) {
    size_t pending = s_queue ? (size_t)uxQueueMessagesWaiting(s_queue) : 0U;
    portENTER_CRITICAL(&s_gate_mux);
    pending += s_overflow_pending;
    portEXIT_CRITICAL(&s_gate_mux);
    return pending;
}

uint32_t event_bus_overflow_count(void) {
    uint32_t count;
    portENTER_CRITICAL(&s_gate_mux);
    count = s_overflow_count;
    portEXIT_CRITICAL(&s_gate_mux);
    return count;
}

bool event_bus_delivery_faulted(void) {
    bool faulted;
    portENTER_CRITICAL(&s_gate_mux);
    faulted = s_delivery_fault;
    portEXIT_CRITICAL(&s_gate_mux);
    return faulted;
}

bool event_bus_begin_sleep_gate(void) {
    if (!s_publish_mutex) return false;
    if (xSemaphoreTake(s_publish_mutex,
                       pdMS_TO_TICKS(EVENT_BUS_PUBLISH_WAIT_MS)) != pdTRUE) {
        return false;
    }
    portENTER_CRITICAL(&s_gate_mux);
    s_sleep_gate = true;
    s_sleep_abort = false;
    portEXIT_CRITICAL(&s_gate_mux);
    xSemaphoreGive(s_publish_mutex);

    for (;;) {
        uint32_t active;
        portENTER_CRITICAL(&s_gate_mux);
        active = s_isr_publishers;
        portEXIT_CRITICAL(&s_gate_mux);
        if (active == 0U) break;
        vTaskDelay(1);
    }
    return true;
}

void event_bus_cancel_sleep_gate(void) {
    portENTER_CRITICAL(&s_gate_mux);
    s_sleep_gate = false;
    s_sleep_abort = false;
    portEXIT_CRITICAL(&s_gate_mux);
}

bool event_bus_sleep_abort_requested(void) {
    bool abort;
    portENTER_CRITICAL(&s_gate_mux);
    abort = s_sleep_abort;
    portEXIT_CRITICAL(&s_gate_mux);
    return abort;
}
