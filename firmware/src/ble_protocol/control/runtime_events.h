#ifndef RUNTIME_EVENTS_H
#define RUNTIME_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Events exchanged by the three runtime FSMs.  The pairing/auth FSM in
 * pairing/state.h is a separate, legacy state machine and deliberately uses a
 * different event type.
 */
typedef enum {
    RTEVT_NONE = 0,
    RTEVT_THERMAL_BUTTON_PRESSED,
    RTEVT_PERIODIC_TICK,
    RTEVT_LOAD_GPIO_APPLIED,
    RTEVT_LOAD_GPIO_FAILED,
    RTEVT_SETTLE_ELAPSED,
    RTEVT_MEASURE_TIMEOUT,
    RTEVT_INA_SAMPLE_OK,
    RTEVT_INA_SAMPLE_FAILED,
    RTEVT_BEEP_FINISHED,
    RTEVT_BUZZER_DONE,
    RTEVT_BUZZER_REQUEST_SHUTDOWN,
    RTEVT_BLE_WINDOW_REQUEST,
    RTEVT_BLE_PAIRING_REQUEST,
    RTEVT_BLE_PAIR_TIMEOUT,
    RTEVT_BLE_ADV_REFRESH,
    RTEVT_BLE_READY,
    RTEVT_BLE_START_FAILED,
    RTEVT_BLE_STOPPED,
    RTEVT_BLE_STOP_FAILED,
    RTEVT_CLIENT_CONNECTED,
    RTEVT_CLIENT_DISCONNECTED,
    RTEVT_ADV_COMPLETE,
    RTEVT_DEADLINE_CHECK,
    RTEVT_SLEEP_ABORTED,
    RTEVT_ACTION_FAILED,
    RTEVT_EVENT_BUS_FAULT,
} runtime_event_kind_t;

#define RUNTIME_ERROR_ACTION_REJECTED (-1)

typedef enum {
    MEASURE_PERIODIC = 0,
    MEASURE_STARTUP,
    MEASURE_SHUTDOWN,
} measure_context_t;

typedef enum {
    BUZZER_OP_STARTUP = 0,
    BUZZER_OP_SHUTDOWN,
    BUZZER_OP_WARNING,
} buzzer_operation_t;

typedef enum {
    BUZZER_PATTERN_NONE = 0,
    BUZZER_PATTERN_NORMAL,
    BUZZER_PATTERN_LOW_BATTERY,
    BUZZER_PATTERN_WARNING,
    BUZZER_PATTERN_ERROR,
} buzzer_pattern_t;

typedef struct {
    runtime_event_kind_t kind;
    uint32_t operation_id;      /* owning load/buzzer/BLE operation, 0 if none */
    uint32_t request_id;        /* correlated asynchronous request, 0 if none */
    measure_context_t context;
    buzzer_operation_t buzzer_operation;
    int64_t timestamp_us;       /* monotonic wall clock microseconds */
    float voltage_v;
    float current_ma;
    int32_t error;
    int32_t command_kind;        /* runtime_command_kind_t for ACTION_FAILED */
    uint16_t conn_handle;
    uint16_t reason;
    bool bool_value;
} runtime_event_t;

/* Unique within uptime (and reset for host tests). */
uint32_t runtime_event_next_id(void);
void runtime_event_reset_ids(uint32_t next_id);

static inline runtime_event_t runtime_event_make(runtime_event_kind_t kind,
                                                 int64_t timestamp_us) {
    runtime_event_t event = {0};
    event.kind = kind;
    event.timestamp_us = timestamp_us;
    event.conn_handle = UINT16_MAX;
    return event;
}

#endif
