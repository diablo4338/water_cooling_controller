#ifndef FREERTOS_QUEUE_H
#define FREERTOS_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

typedef struct host_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *higher_priority_task_woken);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);

#endif
