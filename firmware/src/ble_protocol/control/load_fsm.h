#ifndef LOAD_FSM_H
#define LOAD_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime_commands.h"
#include "runtime_events.h"

typedef enum {
    LOAD_FSM_IDLE = 0,
    LOAD_FSM_STARTING,
    LOAD_FSM_WORKING,
    LOAD_FSM_STOPPING,
    LOAD_FSM_ERROR,
    LOAD_FSM_NEED_CHARGING,
} load_fsm_state_t;

typedef enum {
    LOAD_CHARGE_NONE = 0,
    LOAD_CHARGE_TURNING_ON,
    LOAD_CHARGE_ON,
    LOAD_CHARGE_MEASURING,
    LOAD_CHARGE_RETRY_MEASUREMENT,
    LOAD_CHARGE_TURNING_OFF,
    LOAD_CHARGE_OFF,
} load_charging_phase_t;

typedef struct {
    float low_current_ma;
    float low_voltage_v;
    int64_t low_current_timeout_us;
    int64_t need_charging_confirm_us;
    int64_t measure_timeout_us;
    uint16_t charging_blink_interval_ms;
} load_fsm_config_t;

typedef struct {
    load_fsm_state_t state;
    bool output_on;             /* confirmed by write result or GPIO readback */
    bool off_requested;
    bool verification_pending;
    bool low_current_pending;
    bool low_voltage_current_pending;
    bool charging_measure_pending;
    bool wheel_stopped;
    int64_t charging_end_us;
    int64_t low_current_since_us;
    int64_t low_voltage_current_since_us;
    int64_t charging_deadline_us;
    uint32_t operation_id;      /* current startup or shutdown operation */
    uint32_t startup_operation_id;
    uint32_t shutdown_operation_id;
    uint32_t charging_request_id;
    load_charging_phase_t charging_phase;
    uint8_t off_retries;
    uint8_t on_retries;
    uint8_t verification_retries;
    int64_t action_deadline_us;
    int64_t feedback_deadline_us;
} load_fsm_t;

void load_fsm_init(load_fsm_t *fsm, bool restored_working,
                   bool low_current_pending, int64_t low_current_since_us);

/* Restore wall-clock confirmation history after a short Deep Sleep cycle. */
void load_fsm_restore_charging_confirmation(load_fsm_t *fsm, bool pending,
                                            int64_t since_us);

void load_fsm_step(load_fsm_t *fsm, const runtime_event_t *event,
                   const load_fsm_config_t *config,
                   int64_t now_us,
                   runtime_command_list_t *commands);

/* Only stable ON/OFF phases may be restored or put to sleep. */
bool load_fsm_charging_sleepable(const load_fsm_t *fsm);
void load_fsm_restore_charging(load_fsm_t *fsm, bool output_on,
                               int64_t phase_deadline_us, int64_t end_us);

const char *load_fsm_state_name(load_fsm_state_t state);

#endif
