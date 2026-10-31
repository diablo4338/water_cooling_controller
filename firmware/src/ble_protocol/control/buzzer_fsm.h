#ifndef BUZZER_FSM_H
#define BUZZER_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime_commands.h"
#include "runtime_events.h"

#define BUZZER_SETTLE_MS 1000U
#define BUZZER_MEASURE_TIMEOUT_MS 1500U
#define BUZZER_ERROR_ON_MS 150U
#define BUZZER_ERROR_OFF_MS 150U
#define BUZZER_ERROR_REPEATS 3U

typedef enum {
    BUZZER_FSM_IDLE = 0,
    BUZZER_FSM_WAIT_SETTLE,
    BUZZER_FSM_MEASURING,
    BUZZER_FSM_BEEPING,
} buzzer_fsm_state_t;

typedef struct {
    bool enabled;
    uint16_t threshold_mv;
    uint16_t low_delay_s;
    uint16_t button_ms;
    uint16_t low_ms;
    uint16_t combined_ms;
    uint32_t settle_ms;
    uint32_t measure_timeout_ms;
    uint16_t error_on_ms;
    uint16_t error_off_ms;
    uint8_t error_repeats;
} buzzer_fsm_config_t;

typedef struct {
    buzzer_fsm_state_t state;
    buzzer_operation_t operation;
    uint32_t operation_id;
    uint32_t request_id;
    buzzer_pattern_t pattern;
    int64_t deadline_us;
    uint16_t threshold_mv;
    bool low_pending;
    bool low_alerted;
    int64_t low_since_us;
    bool pending_startup;
    uint32_t pending_startup_id;
    uint32_t last_startup_operation_id;
    uint32_t last_shutdown_operation_id;
} buzzer_fsm_t;

void buzzer_fsm_init(buzzer_fsm_t *fsm, const buzzer_fsm_config_t *config,
                     bool low_pending, bool low_alerted, int64_t low_since_us);

void buzzer_fsm_step(buzzer_fsm_t *fsm, const runtime_event_t *event,
                     const buzzer_fsm_config_t *config,
                     int64_t now_us,
                     runtime_command_list_t *commands);

bool buzzer_fsm_is_active(const buzzer_fsm_t *fsm);
const char *buzzer_fsm_state_name(buzzer_fsm_state_t state);

#endif
