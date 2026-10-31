#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef struct {
    int unused;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux) ((void)(mux))
#define portYIELD_FROM_ISR() ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS pdTRUE
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))

#endif
