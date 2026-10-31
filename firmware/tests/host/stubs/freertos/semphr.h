#ifndef FREERTOS_SEMPHR_H
#define FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct host_semaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);

#endif
