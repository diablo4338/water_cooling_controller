#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreate(void (*task)(void *), const char *name,
                       uint32_t stack_depth, void *arg, UBaseType_t priority,
                       void *task_handle);

#endif
