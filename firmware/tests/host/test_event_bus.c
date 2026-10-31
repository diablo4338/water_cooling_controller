#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event_bus.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

struct host_queue {
    size_t capacity;
    size_t item_size;
    size_t head;
    size_t tail;
    size_t count;
    unsigned char *items;
};

struct host_semaphore {
    bool taken;
};

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

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
    (void)wait;
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

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) {
    (void)wait;
    if (!queue || queue->count == 0U) return pdFALSE;
    memcpy(item, queue->items + queue->head * queue->item_size,
           queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    return queue ? (UBaseType_t)queue->count : 0U;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return calloc(1, sizeof(struct host_semaphore));
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait) {
    (void)wait;
    if (!semaphore || semaphore->taken) return pdFALSE;
    semaphore->taken = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    if (!semaphore) return pdFALSE;
    semaphore->taken = false;
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks) {
    (void)ticks;
}

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static runtime_event_t event_with_id(uint32_t id) {
    runtime_event_t event = runtime_event_make(RTEVT_THERMAL_BUTTON_PRESSED, 0);
    event.request_id = id;
    return event;
}

int main(void) {
    CHECK(event_bus_init());

    CHECK(event_bus_begin_sleep_gate());
    runtime_event_t during_sleep = event_with_id(10);
    CHECK(event_bus_publish_isr(&during_sleep));
    CHECK(event_bus_sleep_abort_requested());
    CHECK(event_bus_pending() == 1U);
    event_bus_cancel_sleep_gate();
    runtime_event_t received;
    CHECK(event_bus_receive(&received, 0));
    CHECK(received.request_id == 10U);

    for (uint32_t id = 0; id < EVENT_BUS_DEPTH; ++id) {
        runtime_event_t event = event_with_id(id);
        CHECK(event_bus_publish_isr(&event));
    }
    runtime_event_t overflow = event_with_id(EVENT_BUS_DEPTH);
    CHECK(event_bus_publish_isr(&overflow));
    CHECK(event_bus_overflow_count() > 0U);
    CHECK(event_bus_pending() == EVENT_BUS_DEPTH + 1U);
    CHECK(!event_bus_delivery_faulted());
    for (uint32_t id = 0; id <= EVENT_BUS_DEPTH; ++id) {
        CHECK(event_bus_receive(&received, 0));
    }
    CHECK(!event_bus_delivery_faulted());

    for (uint32_t id = 0; id < EVENT_BUS_DEPTH + 16U; ++id) {
        runtime_event_t event = event_with_id(id);
        (void)event_bus_publish_isr(&event);
    }
    CHECK(event_bus_delivery_faulted());

    if (failures != 0) {
        fprintf(stderr, "%d event-bus assertions failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("event bus tests passed");
    return EXIT_SUCCESS;
}
