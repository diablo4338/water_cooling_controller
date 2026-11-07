#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "load_fsm.h"
#include "power_manager.h"

static const load_fsm_config_t config = {
    .low_current_ma = 100.0f,
    .low_voltage_v = 6.6f,
    .low_current_timeout_us = 0,
    .need_charging_confirm_us = 5000000,
    .measure_timeout_us = 1500000,
    .charging_blink_interval_ms = 1000,
};

static runtime_command_list_t step(load_fsm_t *fsm, runtime_event_t event,
                                   int64_t now_us) {
    runtime_command_list_t commands;
    runtime_command_list_init(&commands);
    event.timestamp_us = now_us;
    load_fsm_step(fsm, &event, &config, now_us, &commands);
    return commands;
}

static runtime_event_t event(runtime_event_kind_t kind, int64_t now_us) {
    return runtime_event_make(kind, now_us);
}

static runtime_event_t sample(runtime_event_kind_t kind,
                              measure_context_t context, uint32_t request_id,
                              float voltage_v, float current_ma,
                              int64_t now_us) {
    runtime_event_t result = event(kind, now_us);
    result.context = context;
    result.request_id = request_id;
    result.voltage_v = voltage_v;
    result.current_ma = current_ma;
    return result;
}

static runtime_command_t find_command(const runtime_command_list_t *commands,
                                      runtime_command_kind_t kind) {
    for (uint8_t i = 0; i < commands->count; ++i) {
        if (commands->items[i].kind == kind) return commands->items[i];
    }
    assert(!"expected command missing");
    return (runtime_command_t){0};
}

static int count_command(const runtime_command_list_t *commands,
                         runtime_command_kind_t kind) {
    int count = 0;
    for (uint8_t i = 0; i < commands->count; ++i) {
        if (commands->items[i].kind == kind) ++count;
    }
    return count;
}

static void qualify_need_charging(load_fsm_t *fsm) {
    load_fsm_init(fsm, true, false, 0);
    runtime_event_t low = sample(RTEVT_INA_SAMPLE_OK, MEASURE_PERIODIC, 1,
                                 6.0f, 50.0f, 1000000);
    runtime_command_list_t commands = step(fsm, low, 1000000);
    assert(fsm->state == LOAD_FSM_WORKING);
    assert(fsm->low_voltage_current_pending);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);

    low.request_id = 2;
    low.timestamp_us = 6000000;
    commands = step(fsm, low, 6000000);
    assert(fsm->state == LOAD_FSM_NEED_CHARGING);
    assert(fsm->charging_phase == LOAD_CHARGE_TURNING_ON);
    assert(!fsm->low_current_pending);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_ON) == 1);
    assert(count_command(&commands, RCMD_BUZZER_PLAY) == 0);
}

static void acknowledge(load_fsm_t *fsm, bool on, uint32_t operation_id,
                       int64_t now_us) {
    runtime_event_t ack = event(RTEVT_LOAD_GPIO_APPLIED, now_us);
    ack.operation_id = operation_id;
    ack.bool_value = on;
    (void)step(fsm, ack, now_us);
}

static runtime_command_t request_charge_measurement(load_fsm_t *fsm,
                                                    int64_t now_us) {
    runtime_event_t deadline = event(RTEVT_DEADLINE_CHECK, now_us);
    runtime_command_list_t commands = step(fsm, deadline, now_us);
    assert(fsm->charging_phase == LOAD_CHARGE_MEASURING);
    assert(count_command(&commands, RCMD_START_MEASURE_TIMEOUT) == 1);
    assert(count_command(&commands, RCMD_INA_REQUEST) == 1);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);
    return find_command(&commands, RCMD_INA_REQUEST);
}

static void test_confirmation_and_timer_reset_conditions(void) {
    load_fsm_t fsm;
    load_fsm_init(&fsm, true, false, 0);
    runtime_event_t low = sample(RTEVT_INA_SAMPLE_OK, MEASURE_PERIODIC, 1,
                                 6.0f, 50.0f, 1000000);
    (void)step(&fsm, low, 1000000);
    low.timestamp_us = 5900000;
    (void)step(&fsm, low, 5900000);
    assert(fsm.state == LOAD_FSM_WORKING);

    low.timestamp_us = 6000000;
    low.voltage_v = 6.7f;
    (void)step(&fsm, low, 6000000);
    assert(!fsm.low_voltage_current_pending);
    low.timestamp_us = 7000000;
    low.voltage_v = 6.0f;
    low.current_ma = 50.0f;
    (void)step(&fsm, low, 7000000);
    runtime_event_t failed =
        sample(RTEVT_INA_SAMPLE_FAILED, MEASURE_PERIODIC, 3, NAN, NAN, 8000000);
    (void)step(&fsm, failed, 8000000);
    assert(!fsm.low_voltage_current_pending);
    low.timestamp_us = 13000000;
    (void)step(&fsm, low, 13000000);
    assert(fsm.state == LOAD_FSM_WORKING);
    low.timestamp_us = 18000000;
    low.current_ma = 150.0f;
    (void)step(&fsm, low, 18000000);
    assert(!fsm.low_voltage_current_pending);
    assert(fsm.state == LOAD_FSM_WORKING);
}

static void test_low_measurement_precedes_off_and_repeated_cycle(void) {
    load_fsm_t fsm;
    qualify_need_charging(&fsm);
    uint32_t first_on = fsm.operation_id;
    acknowledge(&fsm, true, first_on, 6000000);
    assert(fsm.output_on && fsm.charging_phase == LOAD_CHARGE_ON);

    runtime_command_t request = request_charge_measurement(&fsm, 7000000);
    runtime_event_t low = sample(RTEVT_INA_SAMPLE_OK, MEASURE_NEED_CHARGING,
                                 request.request_id, 6.0f, 50.0f, 7000001);
    runtime_command_list_t commands = step(&fsm, low, 7000001);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING);
    assert(fsm.charging_phase == LOAD_CHARGE_TURNING_OFF);
    runtime_command_t off =
        find_command(&commands, RCMD_LOAD_OUTPUT_OFF);
    assert(off.operation_id == fsm.operation_id);

    acknowledge(&fsm, false, off.operation_id, 7000002);
    assert(!fsm.output_on && fsm.charging_phase == LOAD_CHARGE_OFF);
    runtime_event_t deadline = event(RTEVT_DEADLINE_CHECK, 8000002);
    commands = step(&fsm, deadline, 8000002);
    runtime_command_t next_on =
        find_command(&commands, RCMD_LOAD_OUTPUT_ON);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING);
    acknowledge(&fsm, true, next_on.operation_id, 8000003);
    request = request_charge_measurement(&fsm, 9000003);
    runtime_event_t high = sample(RTEVT_INA_SAMPLE_OK, MEASURE_NEED_CHARGING,
                                  request.request_id, 5.5f, 125.0f, 9000004);
    commands = step(&fsm, high, 9000004);
    assert(fsm.state == LOAD_FSM_WORKING);
    assert(fsm.output_on);
    assert(!fsm.low_current_pending && !fsm.low_voltage_current_pending);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);

    runtime_event_t stale_timeout = event(RTEVT_MEASURE_TIMEOUT, 9500000);
    stale_timeout.context = MEASURE_NEED_CHARGING;
    stale_timeout.request_id = request.request_id;
    commands = step(&fsm, stale_timeout, 9500000);
    assert(fsm.state == LOAD_FSM_WORKING && fsm.output_on);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);

    low = sample(RTEVT_INA_SAMPLE_OK, MEASURE_PERIODIC, 9,
                 6.0f, 50.0f, 10000000);
    (void)step(&fsm, low, 10000000);
    low.timestamp_us = 15000000;
    commands = step(&fsm, low, 15000000);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_ON) == 1);
}

static void test_invalid_and_failed_measurements_never_turn_off(void) {
    load_fsm_t fsm;
    qualify_need_charging(&fsm);
    uint32_t on_id = fsm.operation_id;
    acknowledge(&fsm, true, on_id, 6000000);
    runtime_command_t request = request_charge_measurement(&fsm, 7000000);

    runtime_event_t invalid = sample(RTEVT_INA_SAMPLE_OK,
                                     MEASURE_NEED_CHARGING,
                                     request.request_id, NAN, 20.0f, 7000001);
    runtime_command_list_t commands = step(&fsm, invalid, 7000001);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING && fsm.output_on);
    assert(fsm.charging_phase == LOAD_CHARGE_RETRY_MEASUREMENT);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);

    runtime_event_t deadline = event(RTEVT_DEADLINE_CHECK, 8000001);
    commands = step(&fsm, deadline, 8000001);
    runtime_command_t retry = find_command(&commands, RCMD_INA_REQUEST);
    assert(retry.request_id != request.request_id);
    assert(fsm.output_on && fsm.state == LOAD_FSM_NEED_CHARGING);
    runtime_event_t failed = sample(RTEVT_INA_SAMPLE_FAILED,
                                    MEASURE_NEED_CHARGING,
                                    retry.request_id, NAN, NAN, 8000002);
    commands = step(&fsm, failed, 8000002);
    assert(fsm.charging_phase == LOAD_CHARGE_RETRY_MEASUREMENT);
    assert(fsm.output_on);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);
}

static void test_threshold_equality_does_not_recover(void) {
    load_fsm_t fsm;
    qualify_need_charging(&fsm);
    acknowledge(&fsm, true, fsm.operation_id, 6000000);
    runtime_command_t request = request_charge_measurement(&fsm, 7000000);
    runtime_event_t equal = sample(RTEVT_INA_SAMPLE_OK,
                                   MEASURE_NEED_CHARGING, request.request_id,
                                   6.0f, config.low_current_ma, 7000001);
    runtime_command_list_t commands = step(&fsm, equal, 7000001);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING);
    assert(fsm.charging_phase == LOAD_CHARGE_TURNING_OFF);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 1);
}

static void test_timeout_and_failure_retry_without_off(void) {
    load_fsm_t fsm;
    qualify_need_charging(&fsm);
    acknowledge(&fsm, true, fsm.operation_id, 6000000);
    runtime_command_t request = request_charge_measurement(&fsm, 7000000);
    runtime_event_t timeout = event(RTEVT_MEASURE_TIMEOUT, 8500000);
    timeout.context = MEASURE_NEED_CHARGING;
    timeout.request_id = request.request_id;
    runtime_command_list_t commands = step(&fsm, timeout, 8500000);
    assert(fsm.output_on && fsm.state == LOAD_FSM_NEED_CHARGING);
    assert(fsm.charging_phase == LOAD_CHARGE_RETRY_MEASUREMENT);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);
    runtime_event_t fail = sample(RTEVT_INA_SAMPLE_FAILED,
                                  MEASURE_NEED_CHARGING,
                                  request.request_id, NAN, NAN, 8500001);
    (void)step(&fsm, fail, 8500001);
    assert(fsm.charging_phase == LOAD_CHARGE_RETRY_MEASUREMENT);
    assert(fsm.output_on);
}

static void test_measurement_timeout_setup_failure_is_bounded(void) {
    load_fsm_t fsm;
    qualify_need_charging(&fsm);
    acknowledge(&fsm, true, fsm.operation_id, 6000000);
    runtime_command_t request = request_charge_measurement(&fsm, 7000000);

    runtime_event_t failed = event(RTEVT_ACTION_FAILED, 7000001);
    failed.operation_id = fsm.operation_id;
    failed.command_kind = RCMD_START_MEASURE_TIMEOUT;
    failed.context = MEASURE_NEED_CHARGING;
    failed.request_id = request.request_id;
    runtime_command_list_t commands = step(&fsm, failed, 7000001);
    assert(fsm.charging_phase == LOAD_CHARGE_RETRY_MEASUREMENT);
    assert(fsm.output_on);
    assert(count_command(&commands, RCMD_LOAD_OUTPUT_OFF) == 0);
}

static void test_stale_ack_gpio_failure_and_sleep_block(void) {
    load_fsm_t fsm;
    qualify_need_charging(&fsm);
    uint32_t first_on = fsm.operation_id;
    runtime_event_t stale = event(RTEVT_LOAD_GPIO_APPLIED, 6000000);
    stale.operation_id = first_on + 1U;
    stale.bool_value = false;
    (void)step(&fsm, stale, 6000000);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING && fsm.output_on);

    acknowledge(&fsm, true, first_on, 6000000);
    runtime_command_t request = request_charge_measurement(&fsm, 7000000);
    runtime_event_t low = sample(RTEVT_INA_SAMPLE_OK, MEASURE_NEED_CHARGING,
                                 request.request_id, 6.0f, 50.0f, 7000001);
    runtime_command_list_t commands = step(&fsm, low, 7000001);
    runtime_command_t off = find_command(&commands, RCMD_LOAD_OUTPUT_OFF);
    for (int i = 0; i < 4; ++i) {
        runtime_event_t failed = event(RTEVT_LOAD_GPIO_FAILED, 7000002 + i);
        failed.operation_id = off.operation_id;
        failed.bool_value = false;
        commands = step(&fsm, failed, 7000002 + i);
    }
    assert(fsm.state == LOAD_FSM_ERROR);
    assert(fsm.output_on);

    power_snapshot_t snapshot = {0};
    snapshot.ble_state = BLE_FSM_OFF;
    snapshot.load_state = LOAD_FSM_NEED_CHARGING;
    snapshot.buzzer_state = BUZZER_FSM_IDLE;
    assert(power_manager_sleep_block_reason(&snapshot) ==
           POWER_BLOCK_NEED_CHARGING);
}

static void test_gpio_off_retry_keeps_charging_safe(void) {
    load_fsm_t fsm;
    qualify_need_charging(&fsm);
    uint32_t on_id = fsm.operation_id;
    acknowledge(&fsm, true, on_id, 6000000);

    runtime_command_t request = request_charge_measurement(&fsm, 7000000);
    runtime_event_t low = sample(RTEVT_INA_SAMPLE_OK,
                                 MEASURE_NEED_CHARGING, request.request_id,
                                 6.0f, 50.0f, 7000001);
    runtime_command_list_t commands = step(&fsm, low, 7000001);
    runtime_command_t off = find_command(&commands, RCMD_LOAD_OUTPUT_OFF);
    uint32_t off_id = off.operation_id;

    runtime_event_t failed = event(RTEVT_LOAD_GPIO_FAILED, 7000002);
    failed.operation_id = off_id;
    failed.bool_value = false;
    commands = step(&fsm, failed, 7000002);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING && fsm.output_on);
    runtime_command_t retry = find_command(&commands, RCMD_LOAD_OUTPUT_OFF);
    assert(retry.kind == RCMD_LOAD_OUTPUT_OFF);
    assert(retry.operation_id == off_id);

    acknowledge(&fsm, false, off_id, 7000003);
    assert(fsm.state == LOAD_FSM_NEED_CHARGING && !fsm.output_on);
}

int main(void) {
    test_confirmation_and_timer_reset_conditions();
    test_low_measurement_precedes_off_and_repeated_cycle();
    test_invalid_and_failed_measurements_never_turn_off();
    test_threshold_equality_does_not_recover();
    test_timeout_and_failure_retry_without_off();
    test_measurement_timeout_setup_failure_is_bounded();
    test_stale_ack_gpio_failure_and_sleep_block();
    test_gpio_off_retry_keeps_charging_safe();
    puts("load charging FSM tests passed");
    return 0;
}
