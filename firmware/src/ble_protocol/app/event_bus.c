#include "event_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "event-bus";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_publish_mutex;
static SemaphoreHandle_t s_dispatch_mutex;
static portMUX_TYPE s_gate_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sleep_gate;
static bool s_sleep_abort;
static uint32_t s_overflow_count;

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
        ++s_overflow_count;
        return false;
    }

    bool gate;
    portENTER_CRITICAL(&s_gate_mux);
    gate = s_sleep_gate;
    if (gate) s_sleep_abort = true;
    portEXIT_CRITICAL(&s_gate_mux);

    bool sent = false;
    if (!gate) {
        sent = xQueueSend(s_queue, event, 0) == pdTRUE;
        if (!sent) ++s_overflow_count;
    }
    xSemaphoreGive(s_publish_mutex);
    if (!sent) {
        ESP_LOGW(TAG, "event %d rejected (gate=%d)", (int)event->kind, (int)gate);
    }
    return sent;
}

bool event_bus_publish_isr(const runtime_event_t *event) {
    if (!event || !s_queue) return false;
    BaseType_t woken = pdFALSE;
    bool sent = xQueueSendFromISR(s_queue, event, &woken) == pdTRUE;
    if (!sent) ++s_overflow_count;
    if (woken == pdTRUE) portYIELD_FROM_ISR();
    return sent;
}

bool event_bus_receive(runtime_event_t *event, uint32_t timeout_ms) {
    if (!event || !s_queue) return false;
    return xQueueReceive(s_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

size_t event_bus_pending(void) {
    return s_queue ? (size_t)uxQueueMessagesWaiting(s_queue) : 0U;
}

uint32_t event_bus_overflow_count(void) {
    return s_overflow_count;
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
    return true;
}

void event_bus_cancel_sleep_gate(void) {
    if (!s_publish_mutex) return;
    if (xSemaphoreTake(s_publish_mutex,
                       pdMS_TO_TICKS(EVENT_BUS_PUBLISH_WAIT_MS)) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_gate_mux);
    s_sleep_gate = false;
    s_sleep_abort = false;
    portEXIT_CRITICAL(&s_gate_mux);
    xSemaphoreGive(s_publish_mutex);
}

bool event_bus_sleep_abort_requested(void) {
    bool abort;
    portENTER_CRITICAL(&s_gate_mux);
    abort = s_sleep_abort;
    portEXIT_CRITICAL(&s_gate_mux);
    return abort;
}
