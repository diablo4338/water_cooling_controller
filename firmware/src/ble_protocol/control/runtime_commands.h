#ifndef RUNTIME_COMMANDS_H
#define RUNTIME_COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime_events.h"

/* Matches NimBLE's BLE_HS_FOREVER sentinel for GAP advertising duration. */
#define RUNTIME_BLE_ADVERTISING_FOREVER_MS ((uint32_t)INT32_MAX)

typedef enum {
    RCMD_NONE = 0,
    RCMD_LOAD_OUTPUT_ON,
    RCMD_LOAD_OUTPUT_OFF,
    RCMD_START_SETTLE_TIMER,
    RCMD_CANCEL_SETTLE_TIMER,
    RCMD_INA_REQUEST,
    RCMD_START_MEASURE_TIMEOUT,
    RCMD_CANCEL_MEASURE_TIMEOUT,
    RCMD_BUZZER_PLAY,
    RCMD_BUZZER_CANCEL,
    RCMD_BUZZER_REQUEST_SHUTDOWN,
    RCMD_EMIT_BUZZER_DONE,
    RCMD_BLE_START,
    RCMD_BLE_START_ADVERTISING,
    RCMD_BLE_REFRESH_ADVERTISING,
    RCMD_BLE_DISCONNECT,
    RCMD_ENTER_DEEP_SLEEP,
    RCMD_LOAD_OUTPUT_VERIFY,
} runtime_command_kind_t;

typedef struct {
    runtime_command_kind_t kind;
    uint32_t operation_id;
    uint32_t request_id;
    measure_context_t context;
    buzzer_operation_t buzzer_operation;
    buzzer_pattern_t pattern;
    uint32_t duration_ms;
    uint16_t off_ms;
    uint8_t repeats;
    uint16_t conn_handle;
    int32_t error;
    bool bool_value;
} runtime_command_t;

#define RUNTIME_COMMAND_MAX 12

typedef struct {
    runtime_command_t items[RUNTIME_COMMAND_MAX];
    uint8_t count;
} runtime_command_list_t;

static inline void runtime_command_list_init(runtime_command_list_t *list) {
    list->count = 0;
}

static inline bool runtime_command_list_push(runtime_command_list_t *list,
                                             const runtime_command_t *command) {
    if (!list || !command || list->count >= RUNTIME_COMMAND_MAX) return false;
    list->items[list->count++] = *command;
    return true;
}

static inline runtime_command_t runtime_command_make(runtime_command_kind_t kind,
                                                     uint32_t operation_id) {
    runtime_command_t command = {0};
    command.kind = kind;
    command.operation_id = operation_id;
    return command;
}

#endif
