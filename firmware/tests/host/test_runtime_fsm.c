#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble_fsm.h"
#include "buzzer_fsm.h"
#include "connect_window.h"
#include "load_fsm.h"
#include "load_state.h"
#include "power_manager.h"
#include "runtime_commands.h"
#include "runtime_core.h"
#include "runtime_events.h"

static int s_failures;
static const char *s_current_test = "";

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        long _actual = (long)(actual);                                       \
        long _expected = (long)(expected);                                   \
        if (_actual != _expected) {                                          \
            printf("  CHECK FAILED %s:%d: %s=%ld expected %ld\n",            \
                   __FILE__, __LINE__, #actual, _actual, _expected);         \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                              \
    do {                                                                     \
        double _actual = (double)(actual);                                   \
        double _expected = (double)(expected);                               \
        if (fabs(_actual - _expected) > (double)(tolerance)) {               \
            printf("  CHECK FAILED %s:%d: %s=%.6f expected %.6f\n",          \
                   __FILE__, __LINE__, #actual, _actual, _expected);         \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

typedef struct {
    uint32_t request_id;
    uint32_t operation_id;
    measure_context_t context;
} ina_record_t;

typedef struct {
    buzzer_pattern_t pattern;
    uint32_t operation_id;
    uint32_t duration_ms;
    uint16_t off_ms;
    uint8_t repeats;
} beep_record_t;

typedef struct {
    runtime_command_t items[512];
    size_t count;

    int load_on_count;
    int load_off_count;
    uint32_t last_load_on_op;
    uint32_t last_load_off_op;
    size_t last_load_off_index;

    int settle_count;
    uint32_t settle_operation_id;
    uint32_t settle_ms;

    int ina_count;
    ina_record_t ina[64];

    int measure_timeout_count;

    int beep_count;
    beep_record_t beeps[32];

    int ble_start_count;
    int ble_stop_count;
    int ble_adv_start_count;
    int ble_adv_refresh_count;
    int ble_disconnect_count;
    uint32_t last_ble_start_op;
    uint32_t last_ble_stop_op;
    uint32_t last_adv_request_id;
    uint32_t last_adv_duration_ms;
} mock_t;

static const load_fsm_config_t k_load_config = {
    .low_current_ma = 100.0f,
    .low_current_timeout_us = 300000000LL,
};

static const buzzer_fsm_config_t k_buzzer_config = {
    .enabled = true,
    .threshold_mv = 7000,
    .low_delay_s = 10,
    .button_ms = 500,
    .low_ms = 1000,
    .combined_ms = 1500,
    .settle_ms = 1000,
    .measure_timeout_ms = 1500,
    .error_on_ms = 150,
    .error_off_ms = 150,
    .error_repeats = 3,
};

static const ble_fsm_config_t k_ble_config = {
    .window_us = 60000000LL,
    .max_stop_retries = 2,
};

static runtime_event_t make_event(runtime_event_kind_t kind, int64_t now_us) {
    return runtime_event_make(kind, now_us);
}

static runtime_event_t load_ack_event(bool on, uint32_t operation_id,
                                      int64_t now_us) {
    runtime_event_t event = make_event(
        on ? RTEVT_LOAD_GPIO_APPLIED : RTEVT_LOAD_GPIO_APPLIED, now_us);
    event.operation_id = operation_id;
    event.bool_value = on;
    return event;
}

static runtime_event_t load_failed_event(bool on, uint32_t operation_id,
                                         int64_t now_us) {
    runtime_event_t event = make_event(RTEVT_LOAD_GPIO_FAILED, now_us);
    event.operation_id = operation_id;
    event.bool_value = on;
    return event;
}

static runtime_event_t settle_event(uint32_t operation_id, int64_t now_us) {
    runtime_event_t event = make_event(RTEVT_SETTLE_ELAPSED, now_us);
    event.operation_id = operation_id;
    return event;
}

static runtime_event_t ina_event(bool ok, uint32_t request_id,
                                 uint32_t operation_id,
                                 measure_context_t context,
                                 float voltage_v, float current_ma,
                                 int64_t now_us) {
    runtime_event_t event = make_event(
        ok ? RTEVT_INA_SAMPLE_OK : RTEVT_INA_SAMPLE_FAILED, now_us);
    event.request_id = request_id;
    event.operation_id = operation_id;
    event.context = context;
    event.voltage_v = voltage_v;
    event.current_ma = current_ma;
    return event;
}

static runtime_event_t timeout_event(uint32_t request_id,
                                     uint32_t operation_id,
                                     measure_context_t context,
                                     int64_t now_us) {
    runtime_event_t event = make_event(RTEVT_MEASURE_TIMEOUT, now_us);
    event.request_id = request_id;
    event.operation_id = operation_id;
    event.context = context;
    return event;
}

static runtime_event_t beep_finished_event(uint32_t operation_id,
                                           int64_t now_us) {
    runtime_event_t event = make_event(RTEVT_BEEP_FINISHED, now_us);
    event.operation_id = operation_id;
    return event;
}

static void init_core(runtime_core_t *core, bool restored_working,
                      bool buzzer_enabled) {
    buzzer_fsm_config_t buzzer_config = k_buzzer_config;
    buzzer_config.enabled = buzzer_enabled;
    runtime_event_reset_ids(1);
    runtime_core_init(core, &k_load_config, &buzzer_config, &k_ble_config,
                      restored_working, false, 0, false, false, 0);
}

static void mock_record(mock_t *mock, const runtime_command_t *command) {
    if (mock->count < sizeof(mock->items) / sizeof(mock->items[0])) {
        mock->items[mock->count++] = *command;
    }

    switch (command->kind) {
        case RCMD_LOAD_OUTPUT_ON:
            ++mock->load_on_count;
            mock->last_load_on_op = command->operation_id;
            break;
        case RCMD_LOAD_OUTPUT_OFF:
            ++mock->load_off_count;
            mock->last_load_off_op = command->operation_id;
            mock->last_load_off_index = mock->count - 1;
            break;
        case RCMD_START_SETTLE_TIMER:
            ++mock->settle_count;
            mock->settle_operation_id = command->operation_id;
            mock->settle_ms = command->duration_ms;
            break;
        case RCMD_INA_REQUEST:
            if (mock->ina_count <
                (int)(sizeof(mock->ina) / sizeof(mock->ina[0]))) {
                mock->ina[mock->ina_count++] = (ina_record_t) {
                    .request_id = command->request_id,
                    .operation_id = command->operation_id,
                    .context = command->context,
                };
            }
            break;
        case RCMD_START_MEASURE_TIMEOUT:
            ++mock->measure_timeout_count;
            break;
        case RCMD_BUZZER_PLAY:
            if (mock->beep_count <
                (int)(sizeof(mock->beeps) / sizeof(mock->beeps[0]))) {
                mock->beeps[mock->beep_count++] = (beep_record_t) {
                    .pattern = command->pattern,
                    .operation_id = command->operation_id,
                    .duration_ms = command->duration_ms,
                    .off_ms = command->off_ms,
                    .repeats = command->repeats,
                };
            }
            break;
        case RCMD_BLE_START:
            ++mock->ble_start_count;
            mock->last_ble_start_op = command->operation_id;
            break;
        case RCMD_BLE_STOP:
            ++mock->ble_stop_count;
            mock->last_ble_stop_op = command->operation_id;
            break;
        case RCMD_BLE_START_ADVERTISING:
            ++mock->ble_adv_start_count;
            mock->last_adv_request_id = command->request_id;
            mock->last_adv_duration_ms = command->duration_ms;
            break;
        case RCMD_BLE_REFRESH_ADVERTISING:
            ++mock->ble_adv_refresh_count;
            mock->last_adv_request_id = command->request_id;
            mock->last_adv_duration_ms = command->duration_ms;
            break;
        case RCMD_BLE_DISCONNECT:
            ++mock->ble_disconnect_count;
            break;
        default:
            break;
    }
}

static void apply_commands(runtime_core_t *core, mock_t *mock,
                           const runtime_command_list_t *commands,
                           int64_t now_us);

static void step(runtime_core_t *core, mock_t *mock,
                 runtime_event_t event, int64_t now_us) {
    event.timestamp_us = now_us;
    runtime_command_list_t commands;
    runtime_command_list_init(&commands);
    runtime_core_step(core, &event, now_us, &commands);
    apply_commands(core, mock, &commands, now_us);
}

static void ble_ready(runtime_core_t *core, mock_t *mock, int64_t now_us);
static void ble_stopped(runtime_core_t *core, mock_t *mock, int64_t now_us);

static void apply_commands(runtime_core_t *core, mock_t *mock,
                           const runtime_command_list_t *commands,
                           int64_t now_us) {
    runtime_event_t follow_up[RUNTIME_COMMAND_MAX];
    int follow_up_count = 0;

    for (uint8_t i = 0; i < commands->count; ++i) {
        runtime_event_t follow_event;
        if (runtime_core_command_event(&commands->items[i], &follow_event)) {
            if (follow_up_count < RUNTIME_COMMAND_MAX) {
                follow_up[follow_up_count++] = follow_event;
            }
        } else {
            mock_record(mock, &commands->items[i]);
        }
    }

    for (int i = 0; i < follow_up_count; ++i) {
        step(core, mock, follow_up[i], now_us);
    }
}

static void start_to_measuring(runtime_core_t *core, mock_t *mock,
                               uint32_t *operation_id, uint32_t *request_id) {
    step(core, mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    CHECK_INT(mock->load_on_count, 1);
    *operation_id = mock->last_load_on_op;

    step(core, mock, load_ack_event(true, *operation_id, 100), 100);
    CHECK_INT(core->buzzer.state, BUZZER_FSM_WAIT_SETTLE);
    CHECK_INT(mock->settle_count, 1);
    CHECK_INT(mock->settle_ms, 1000);
    CHECK_INT(mock->ina_count, 0);

    step(core, mock, settle_event(*operation_id, 1100), 1100);
    CHECK_INT(core->buzzer.state, BUZZER_FSM_MEASURING);
    CHECK_INT(mock->ina_count, 1);
    CHECK_INT(mock->measure_timeout_count, 1);
    *request_id = mock->ina[mock->ina_count - 1].request_id;
}

static void low_current_shutdown(runtime_core_t *core, mock_t *mock,
                                 bool shutdown_sample_ok) {
    step(core, mock, ina_event(true, 1001, 0, MEASURE_PERIODIC,
                               12.0f, 50.0f, 0), 0);
    step(core, mock, ina_event(true, 1002, 0, MEASURE_PERIODIC,
                               12.0f, 50.0f, 299000000LL), 299000000LL);
    CHECK_INT(core->load.state, LOAD_FSM_WORKING);
    CHECK_INT(mock->load_off_count, 0);

    step(core, mock, ina_event(true, 1003, 0, MEASURE_PERIODIC,
                               12.0f, 50.0f, 300000000LL), 300000000LL);
    CHECK_INT(core->load.state, LOAD_FSM_STOPPING);
    CHECK_INT(core->buzzer.state, BUZZER_FSM_MEASURING);
    CHECK_INT(mock->load_off_count, 0);
    CHECK_INT(mock->beep_count, 0);
    CHECK(mock->ina_count >= 1);
    uint32_t shutdown_op = core->load.shutdown_operation_id;
    uint32_t shutdown_req = mock->ina[mock->ina_count - 1].request_id;
    CHECK_INT(mock->ina[mock->ina_count - 1].context, MEASURE_SHUTDOWN);

    if (shutdown_sample_ok) {
        step(core, mock, ina_event(true, shutdown_req, shutdown_op,
                                   MEASURE_SHUTDOWN, 12.0f, 50.0f,
                                   301000000LL), 301000000LL);
    } else {
        step(core, mock, ina_event(false, shutdown_req, shutdown_op,
                                   MEASURE_SHUTDOWN, NAN, NAN,
                                   301000000LL), 301000000LL);
    }

    CHECK_INT(mock->beep_count, 0);
    CHECK_INT(core->buzzer.state, BUZZER_FSM_IDLE);
    CHECK_INT(core->load.state, LOAD_FSM_STOPPING);
    CHECK_INT(mock->load_off_count, 1);
    CHECK_INT(mock->last_load_off_op, shutdown_op);

    step(core, mock, load_ack_event(false, mock->last_load_off_op,
                                    302000000LL), 302000000LL);
    CHECK_INT(core->load.state, LOAD_FSM_IDLE);
    CHECK(!core->load.output_on);
}

/* ------------------------------------------------------------------ tests */

static void test_load_startup_sequence(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    CHECK_INT(core.load.state, LOAD_FSM_STARTING);
    CHECK_INT(core.ble.state, BLE_FSM_STARTING);
    CHECK_INT(mock.load_on_count, 1);
    CHECK_INT(mock.settle_count, 0);
    CHECK_INT(mock.ina_count, 0);
    CHECK_INT(mock.ble_start_count, 1);
    uint32_t op = mock.last_load_on_op;

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 100000), 100000);
    CHECK_INT(mock.load_on_count, 1);
    CHECK_INT(mock.ble_start_count, 1);
    CHECK_INT(core.ble.state, BLE_FSM_STARTING);

    step(&core, &mock, load_ack_event(true, op, 200000), 200000);
    CHECK_INT(core.load.state, LOAD_FSM_STARTING);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_WAIT_SETTLE);
    CHECK_INT(mock.settle_count, 1);
    CHECK_INT(mock.settle_operation_id, op);
    CHECK_INT(mock.ina_count, 0);

    step(&core, &mock, settle_event(op, 1200000), 1200000);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_MEASURING);
    CHECK_INT(mock.ina_count, 1);
    CHECK_INT(mock.ina[0].context, MEASURE_STARTUP);
    CHECK_INT(mock.measure_timeout_count, 1);
    uint32_t request = mock.ina[0].request_id;

    step(&core, &mock, settle_event(op, 1300000), 1300000);
    CHECK_INT(mock.ina_count, 1);

    step(&core, &mock, ina_event(true, request, op, MEASURE_STARTUP,
                                 12.0f, 50.0f, 1400000), 1400000);
    CHECK_INT(mock.beep_count, 1);
    CHECK_INT(mock.beeps[0].pattern, BUZZER_PATTERN_NORMAL);
    CHECK_INT(mock.beeps[0].duration_ms, 500);
    CHECK_INT(mock.beeps[0].repeats, 1);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_BEEPING);

    step(&core, &mock, ina_event(true, request, op, MEASURE_STARTUP,
                                 12.0f, 50.0f, 1500000), 1500000);
    CHECK_INT(mock.beep_count, 1);

    step(&core, &mock, beep_finished_event(op, 1900000), 1900000);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_IDLE);
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);
    CHECK(core.load.output_on);
    CHECK_INT(mock.load_off_count, 0);

    step(&core, &mock, beep_finished_event(op, 2000000), 2000000);
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);
    CHECK_INT(mock.beep_count, 1);
}

static void test_startup_ina_error_once(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    uint32_t op = 0;
    uint32_t request = 0;
    start_to_measuring(&core, &mock, &op, &request);

    step(&core, &mock, ina_event(false, request, op, MEASURE_STARTUP,
                                 NAN, NAN, 1200000), 1200000);
    CHECK_INT(mock.beep_count, 1);
    CHECK_INT(mock.beeps[0].pattern, BUZZER_PATTERN_ERROR);
    CHECK_INT(mock.beeps[0].duration_ms, 150);
    CHECK_INT(mock.beeps[0].off_ms, 150);
    CHECK_INT(mock.beeps[0].repeats, 3);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_BEEPING);

    step(&core, &mock, ina_event(false, request, op, MEASURE_STARTUP,
                                 NAN, NAN, 1300000), 1300000);
    step(&core, &mock, timeout_event(request, op, MEASURE_STARTUP,
                                     1400000), 1400000);
    CHECK_INT(mock.beep_count, 1);

    step(&core, &mock, beep_finished_event(op, 2000000), 2000000);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_IDLE);
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);

    /* A new operation must produce its own pattern. */
    runtime_core_t second;
    mock_t second_mock;
    memset(&second_mock, 0, sizeof(second_mock));
    init_core(&second, false, true);
    uint32_t second_op = 0;
    uint32_t second_request = 0;
    start_to_measuring(&second, &second_mock, &second_op, &second_request);
    step(&second, &second_mock,
         ina_event(false, second_request, second_op, MEASURE_STARTUP,
                   NAN, NAN, 1200000), 1200000);
    CHECK_INT(second_mock.beep_count, 1);
    CHECK_INT(second_mock.beeps[0].pattern, BUZZER_PATTERN_ERROR);
    CHECK_INT(second_mock.beeps[0].repeats, 3);
}

static void test_startup_voltage_selects_cue(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    uint32_t op = 0;
    uint32_t request = 0;
    start_to_measuring(&core, &mock, &op, &request);
    step(&core, &mock, ina_event(true, request, op, MEASURE_STARTUP,
                                 6.9f, 50.0f, 1200000), 1200000);
    CHECK_INT(mock.beep_count, 1);
    CHECK_INT(mock.beeps[0].pattern, BUZZER_PATTERN_LOW_BATTERY);
    CHECK_INT(mock.beeps[0].duration_ms, 1500);

    runtime_core_t second;
    mock_t second_mock;
    memset(&second_mock, 0, sizeof(second_mock));
    init_core(&second, false, true);
    uint32_t second_op = 0;
    uint32_t second_request = 0;
    start_to_measuring(&second, &second_mock, &second_op, &second_request);
    step(&second, &second_mock,
         ina_event(true, second_request, second_op, MEASURE_STARTUP,
                   7.0f, 50.0f, 1200000), 1200000);
    CHECK_INT(second_mock.beep_count, 1);
    CHECK_INT(second_mock.beeps[0].pattern, BUZZER_PATTERN_NORMAL);
    CHECK_INT(second_mock.beeps[0].duration_ms, 500);
}

static void test_periodic_failures_silent(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, true, true);

    for (int i = 0; i < 100; ++i) {
        step(&core, &mock,
             ina_event(false, 2000 + (uint32_t)i, 0, MEASURE_PERIODIC,
                       NAN, NAN, (int64_t)i * 1000000LL),
             (int64_t)i * 1000000LL);
    }
    CHECK_INT(mock.beep_count, 0);
    CHECK(!core.buzzer.low_pending);
    CHECK(!core.load.low_current_pending);
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);
}

static void test_low_current_shutdown_failure_silent(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, true, true);
    low_current_shutdown(&core, &mock, false);
}

static void test_low_current_shutdown_success_silent(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, true, true);
    low_current_shutdown(&core, &mock, true);
}

static void test_low_voltage_warning_delay(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, true, true);

    step(&core, &mock, ina_event(true, 6001, 0, MEASURE_PERIODIC,
                                 6.5f, 500.0f, 0), 0);
    CHECK_INT(mock.beep_count, 0);
    CHECK(core.buzzer.low_pending);
    CHECK(!core.buzzer.low_alerted);

    step(&core, &mock, ina_event(true, 6002, 0, MEASURE_PERIODIC,
                                 6.5f, 500.0f, 10000000LL), 10000000LL);
    CHECK_INT(mock.beep_count, 0);

    step(&core, &mock, ina_event(true, 6003, 0, MEASURE_PERIODIC,
                                 6.5f, 500.0f, 10000001LL), 10000001LL);
    CHECK_INT(mock.beep_count, 1);
    CHECK_INT(mock.beeps[0].pattern, BUZZER_PATTERN_WARNING);
    CHECK_INT(mock.beeps[0].duration_ms, 1000);
    CHECK(core.buzzer.low_alerted);

    step(&core, &mock, ina_event(true, 6004, 0, MEASURE_PERIODIC,
                                 6.5f, 500.0f, 12000000LL), 12000000LL);
    CHECK_INT(mock.beep_count, 1);

    step(&core, &mock, beep_finished_event(core.buzzer.operation_id,
                                           13000000LL), 13000000LL);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_IDLE);
    step(&core, &mock, ina_event(true, 6005, 0, MEASURE_PERIODIC,
                                 6.5f, 500.0f, 20000000LL), 20000000LL);
    CHECK_INT(mock.beep_count, 1);

    /* Recovery clears the latch; a later low interval warns again. */
    step(&core, &mock, ina_event(true, 6006, 0, MEASURE_PERIODIC,
                                 7.5f, 500.0f, 21000000LL), 21000000LL);
    CHECK(!core.buzzer.low_pending);
    step(&core, &mock, ina_event(true, 6007, 0, MEASURE_PERIODIC,
                                 6.5f, 500.0f, 22000000LL), 22000000LL);
    step(&core, &mock, ina_event(true, 6008, 0, MEASURE_PERIODIC,
                                 6.5f, 500.0f, 32000001LL), 32000001LL);
    CHECK_INT(mock.beep_count, 2);
}

static void test_load_gpio_failure_handling(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    uint32_t start_op = mock.last_load_on_op;
    step(&core, &mock, load_failed_event(true, start_op, 100), 100);
    CHECK_INT(core.load.state, LOAD_FSM_IDLE);
    CHECK(!core.load.output_on);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_IDLE);
    CHECK_INT(mock.settle_count, 0);

    runtime_core_t second;
    mock_t second_mock;
    memset(&second_mock, 0, sizeof(second_mock));
    init_core(&second, true, true);
    step(&second, &second_mock,
         ina_event(true, 5001, 0, MEASURE_PERIODIC, 12.0f, 50.0f, 0), 0);
    step(&second, &second_mock,
         ina_event(true, 5002, 0, MEASURE_PERIODIC, 12.0f, 50.0f,
                   300000000LL), 300000000LL);
    CHECK_INT(second.load.state, LOAD_FSM_STOPPING);
    uint32_t shutdown_op = second.load.shutdown_operation_id;
    uint32_t shutdown_req = second_mock.ina[second_mock.ina_count - 1].request_id;
    step(&second, &second_mock,
         ina_event(true, shutdown_req, shutdown_op, MEASURE_SHUTDOWN,
                   12.0f, 50.0f, 301000000LL), 301000000LL);
    CHECK_INT(second_mock.load_off_count, 1);
    uint32_t off_op = second_mock.last_load_off_op;

    step(&second, &second_mock, load_failed_event(false, off_op,
                                                  302000000LL),
         302000000LL);
    CHECK_INT(second.load.state, LOAD_FSM_STOPPING);
    CHECK_INT(second.load.off_retries, 1);
    CHECK_INT(second_mock.load_off_count, 2);

    step(&second, &second_mock, load_failed_event(false, off_op,
                                                  303000000LL),
         303000000LL);
    CHECK_INT(second.load.off_retries, 2);
    CHECK_INT(second_mock.load_off_count, 3);

    step(&second, &second_mock, load_ack_event(false, off_op, 304000000LL),
         304000000LL);
    CHECK_INT(second.load.state, LOAD_FSM_IDLE);
    CHECK(!second.load.output_on);
}

static void test_invalid_current_resets_low_timer(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, true, true);

    step(&core, &mock, ina_event(true, 3001, 0, MEASURE_PERIODIC,
                                 12.0f, 50.0f, 0), 0);
    step(&core, &mock, ina_event(false, 3002, 0, MEASURE_PERIODIC,
                                 NAN, NAN, 100000000LL), 100000000LL);
    step(&core, &mock, ina_event(true, 3003, 0, MEASURE_PERIODIC,
                                 12.0f, 50.0f, 200000000LL), 200000000LL);
    step(&core, &mock, ina_event(true, 3004, 0, MEASURE_PERIODIC,
                                 12.0f, 50.0f, 499000000LL), 499000000LL);
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);
    step(&core, &mock, ina_event(true, 3005, 0, MEASURE_PERIODIC,
                                 12.0f, 50.0f, 500000000LL), 500000000LL);
    CHECK_INT(core.load.state, LOAD_FSM_STOPPING);
    CHECK_INT(mock.load_off_count, 0);
}

static void test_buzzer_disabled_completes(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, false);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    uint32_t op = mock.last_load_on_op;
    step(&core, &mock, load_ack_event(true, op, 100), 100);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_IDLE);
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);
    CHECK_INT(mock.settle_count, 0);
    CHECK_INT(mock.beep_count, 0);

    step(&core, &mock, ina_event(true, 4001, 0, MEASURE_PERIODIC,
                                 12.0f, 50.0f, 200), 200);
    step(&core, &mock, ina_event(true, 4002, 0, MEASURE_PERIODIC,
                                 12.0f, 50.0f, 300000200LL), 300000200LL);
    CHECK_INT(core.load.state, LOAD_FSM_STOPPING);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_IDLE);
    CHECK_INT(mock.load_off_count, 1);
    CHECK_INT(mock.beep_count, 0);

    step(&core, &mock, load_ack_event(false, mock.last_load_off_op,
                                      300000300LL), 300000300LL);
    CHECK_INT(core.load.state, LOAD_FSM_IDLE);
}

static void test_ble_lazy_start_and_deadline(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_DEADLINE_CHECK, 1000000), 1000000);
    CHECK_INT(mock.ble_start_count, 0);
    CHECK_INT(mock.ble_adv_start_count, 0);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 2000000),
         2000000);
    CHECK_INT(core.ble.state, BLE_FSM_STARTING);
    CHECK_INT(mock.ble_start_count, 1);
    uint32_t op = core.ble.operation_id;

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 12000000),
         12000000);
    CHECK_INT(mock.ble_start_count, 1);
    CHECK_INT(core.ble.window_deadline_us, 72000000LL);

    ble_ready(&core, &mock, 13000000);
    (void)op;
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);
    CHECK_INT(mock.ble_adv_start_count, 1);
    CHECK(mock.last_adv_duration_ms > 0);
    CHECK(mock.last_adv_duration_ms <= 60000);

    step(&core, &mock, make_event(RTEVT_DEADLINE_CHECK, 71000000), 71000000);
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);

    step(&core, &mock, make_event(RTEVT_DEADLINE_CHECK, 72000000), 72000000);
    CHECK_INT(core.ble.state, BLE_FSM_STOPPING);
    CHECK_INT(mock.ble_stop_count, 1);

    ble_stopped(&core, &mock, 73000000);
    CHECK_INT(core.ble.state, BLE_FSM_OFF);
    CHECK_INT(mock.ble_adv_start_count, 1);
}

static void ble_ready(runtime_core_t *core, mock_t *mock, int64_t now_us) {
    runtime_event_t event = make_event(RTEVT_BLE_READY, now_us);
    event.operation_id = core->ble.operation_id;
    step(core, mock, event, now_us);
}

static void ble_stopped(runtime_core_t *core, mock_t *mock, int64_t now_us) {
    runtime_event_t event = make_event(RTEVT_BLE_STOPPED, now_us);
    event.operation_id = core->ble.stop_operation_id;
    step(core, mock, event, now_us);
}

static void connect_current_advertisement(runtime_core_t *core, mock_t *mock,
                                          uint16_t handle, int64_t now_us) {
    runtime_event_t event = make_event(RTEVT_CLIENT_CONNECTED, now_us);
    event.operation_id = core->ble.operation_id;
    event.request_id = core->ble.adv_request_id;
    event.conn_handle = handle;
    step(core, mock, event, now_us);
}

static void test_ble_disconnect_always_stops(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    ble_ready(&core, &mock, 1000000);
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);
    connect_current_advertisement(&core, &mock, 7, 2000000);
    CHECK_INT(core.ble.state, BLE_FSM_CONNECTED);
    CHECK_INT(core.ble.conn_handle, 7);

    step(&core, &mock, make_event(RTEVT_DEADLINE_CHECK, 120000000LL),
         120000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_CONNECTED);

    runtime_event_t wrong = make_event(RTEVT_CLIENT_DISCONNECTED, 121000000LL);
    wrong.conn_handle = 8;
    step(&core, &mock, wrong, 121000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_CONNECTED);

    runtime_event_t disconnect = make_event(RTEVT_CLIENT_DISCONNECTED,
                                            122000000LL);
    disconnect.conn_handle = 7;
    step(&core, &mock, disconnect, 122000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_STOPPING);
    CHECK_INT(mock.ble_stop_count, 1);

    ble_stopped(&core, &mock, 123000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_OFF);
    CHECK_INT(mock.ble_adv_start_count, 1);
}

static void test_ble_stopping_press_restarts_once(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    ble_ready(&core, &mock, 1000000);
    step(&core, &mock, make_event(RTEVT_DEADLINE_CHECK, 60000000LL),
         60000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_STOPPING);
    uint32_t stop_op = core.ble.stop_operation_id;
    CHECK_INT(mock.ble_stop_count, 1);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 61000000LL),
         61000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_STOPPING);
    CHECK(core.ble.pending_restart);
    CHECK_INT(mock.ble_start_count, 1);

    ble_stopped(&core, &mock, 62000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_STARTING);
    CHECK_INT(mock.ble_start_count, 2);
    CHECK(!core.ble.pending_restart);

    runtime_event_t duplicate = make_event(RTEVT_BLE_STOPPED, 63000000LL);
    duplicate.operation_id = stop_op;
    step(&core, &mock, duplicate, 63000000LL);
    CHECK_INT(core.ble.state, BLE_FSM_STARTING);
    CHECK_INT(mock.ble_start_count, 2);
}

static void test_ble_adv_complete_stops_or_restarts(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    ble_ready(&core, &mock, 1000000);
    runtime_event_t complete = make_event(RTEVT_ADV_COMPLETE, 5000000);
    complete.request_id = core.ble.adv_request_id;
    step(&core, &mock, complete, 5000000);
    CHECK_INT(core.ble.state, BLE_FSM_STOPPING);
    CHECK_INT(mock.ble_stop_count, 1);

    runtime_core_t second;
    mock_t second_mock;
    memset(&second_mock, 0, sizeof(second_mock));
    init_core(&second, false, true);
    step(&second, &second_mock,
         make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    ble_ready(&second, &second_mock, 1000000);
    step(&second, &second_mock,
         make_event(RTEVT_THERMAL_BUTTON_PRESSED, 2000000), 2000000);
    runtime_event_t extended_complete = make_event(RTEVT_ADV_COMPLETE,
                                                    60000000LL);
    extended_complete.request_id = second.ble.adv_request_id;
    step(&second, &second_mock, extended_complete, 60000000LL);
    CHECK_INT(second.ble.state, BLE_FSM_ADVERTISING);
    CHECK_INT(second_mock.ble_adv_start_count, 2);
    CHECK_INT(second_mock.ble_stop_count, 0);
}

static void test_ble_late_callbacks_ignored(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    ble_ready(&core, &mock, 1000000);
    uint32_t old_adv = core.ble.adv_request_id;
    uint32_t old_op = core.ble.operation_id;
    connect_current_advertisement(&core, &mock, 9, 2000000);
    runtime_event_t disconnect = make_event(RTEVT_CLIENT_DISCONNECTED,
                                            3000000);
    disconnect.conn_handle = 9;
    step(&core, &mock, disconnect, 3000000);
    uint32_t old_stop = core.ble.stop_operation_id;
    ble_stopped(&core, &mock, 4000000);
    CHECK_INT(core.ble.state, BLE_FSM_OFF);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 5000000),
         5000000);
    ble_ready(&core, &mock, 6000000);
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);
    uint32_t new_adv = core.ble.adv_request_id;
    CHECK(new_adv != old_adv);

    runtime_event_t late_complete = make_event(RTEVT_ADV_COMPLETE, 7000000);
    late_complete.operation_id = old_op;
    late_complete.request_id = old_adv;
    step(&core, &mock, late_complete, 7000000);
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);

    runtime_event_t late_disconnect = make_event(RTEVT_CLIENT_DISCONNECTED,
                                                 8000000);
    late_disconnect.conn_handle = 9;
    step(&core, &mock, late_disconnect, 8000000);
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);

    runtime_event_t late_stop = make_event(RTEVT_BLE_STOPPED, 9000000);
    late_stop.operation_id = old_stop;
    step(&core, &mock, late_stop, 9000000);
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);
    CHECK_INT(mock.ble_stop_count, 1);
}

static void test_ble_pair_timeout(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    ble_ready(&core, &mock, 1000000);
    CHECK_INT(mock.ble_adv_start_count, 1);

    step(&core, &mock, make_event(RTEVT_BLE_PAIR_TIMEOUT, 2000000), 2000000);
    CHECK_INT(core.ble.state, BLE_FSM_ADVERTISING);
    CHECK_INT(mock.ble_adv_refresh_count, 1);

    connect_current_advertisement(&core, &mock, 11, 3000000);
    runtime_event_t timeout = make_event(RTEVT_BLE_PAIR_TIMEOUT, 4000000);
    timeout.bool_value = true;
    step(&core, &mock, timeout, 4000000);
    CHECK_INT(core.ble.state, BLE_FSM_STOPPING);
    CHECK_INT(mock.ble_stop_count, 1);

    runtime_core_t second;
    mock_t second_mock;
    memset(&second_mock, 0, sizeof(second_mock));
    init_core(&second, false, true);
    step(&second, &second_mock,
         make_event(RTEVT_THERMAL_BUTTON_PRESSED, 0), 0);
    ble_ready(&second, &second_mock, 1000000);
    connect_current_advertisement(&second, &second_mock, 12, 2000000);
    runtime_event_t authed_timeout = make_event(RTEVT_BLE_PAIR_TIMEOUT,
                                                3000000);
    authed_timeout.bool_value = false;
    step(&second, &second_mock, authed_timeout, 3000000);
    CHECK_INT(second.ble.state, BLE_FSM_CONNECTED);
    CHECK_INT(second_mock.ble_stop_count, 0);
}

static void test_restored_working_sleepable(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, true, true);

    /* A wake restores WORKING with the output still on, without replaying the
     * startup sequence. */
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);
    CHECK(core.load.output_on);
    CHECK_INT(core.ble.state, BLE_FSM_OFF);
    CHECK_INT(core.buzzer.state, BUZZER_FSM_IDLE);
    CHECK_INT(mock.load_on_count, 0);
    CHECK_INT(mock.settle_count, 0);

    power_snapshot_t snapshot = {
        .ble_state = core.ble.state,
        .load_state = core.load.state,
        .buzzer_state = core.buzzer.state,
        .ble_radio_quiet = true,
    };
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_SLEEP_ALLOWED);

    /* A button press after wake starts only the BLE window; it must not
     * re-activate or toggle the working load. */
    step(&core, &mock, make_event(RTEVT_THERMAL_BUTTON_PRESSED, 1000000),
         1000000);
    CHECK_INT(core.load.state, LOAD_FSM_WORKING);
    CHECK_INT(core.ble.state, BLE_FSM_STARTING);
    CHECK_INT(mock.load_on_count, 0);
    CHECK_INT(mock.load_off_count, 0);
}

static void test_power_manager(void) {
    power_snapshot_t snapshot = {
        .ble_state = BLE_FSM_OFF,
        .load_state = LOAD_FSM_IDLE,
        .buzzer_state = BUZZER_FSM_IDLE,
        .ble_radio_quiet = true,
    };
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_SLEEP_ALLOWED);
    CHECK(power_manager_can_sleep(&snapshot));

    snapshot.load_state = LOAD_FSM_WORKING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_SLEEP_ALLOWED);

    snapshot.load_state = LOAD_FSM_STARTING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot),
              POWER_BLOCK_LOAD_TRANSITION);
    snapshot.load_state = LOAD_FSM_STOPPING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot),
              POWER_BLOCK_LOAD_TRANSITION);
    snapshot.load_state = LOAD_FSM_IDLE;

    snapshot.ble_state = BLE_FSM_STARTING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BLE);
    snapshot.ble_state = BLE_FSM_ADVERTISING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BLE);
    snapshot.ble_state = BLE_FSM_STOPPING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BLE);
    snapshot.ble_state = BLE_FSM_CONNECTED;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BLE);
    snapshot.ble_state = BLE_FSM_OFF;
    snapshot.ble_connected = true;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot),
              POWER_BLOCK_BLE_CONNECTED);
    snapshot.ble_connected = false;

    snapshot.ble_radio_quiet = false;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BLE_RADIO);
    snapshot.ble_radio_quiet = true;

    snapshot.buzzer_state = BUZZER_FSM_WAIT_SETTLE;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BUZZER);
    snapshot.buzzer_state = BUZZER_FSM_MEASURING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BUZZER);
    snapshot.buzzer_state = BUZZER_FSM_BEEPING;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_BUZZER);
    snapshot.buzzer_state = BUZZER_FSM_IDLE;

    snapshot.action_pending = true;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_ACTION);
    snapshot.action_pending = false;
    snapshot.event_pending = true;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot), POWER_BLOCK_EVENT);
    snapshot.event_pending = false;
    snapshot.critical_pending = true;
    CHECK_INT(power_manager_sleep_block_reason(&snapshot),
              POWER_BLOCK_CRITICAL);
}

static void test_thermal_edge_and_window(void) {
    load_state_t state = {0};
    const load_policy_t policy = {
        .button_delta_c = 2.0f,
        .idle_current_ma = 100.0f,
        .idle_timeout_us = 300000000LL,
        .require_new_press = true,
    };
    for (int i = 0; i < 5; ++i) {
        (void)load_state_update_button(&state, &policy, 25.0f);
    }
    CHECK(load_state_update_button(&state, &policy, 27.0f));
    CHECK(!load_state_update_button(&state, &policy, 27.0f));
    CHECK(!load_state_update_button(&state, &policy, NAN));
    CHECK(state.button_was_pressed);
    CHECK(!load_state_update_button(&state, &policy, 25.0f));

    connect_window_t window = {0};
    connect_window_open(&window, 1000000);
    CHECK_INT(connect_window_remaining_ms(&window, 1000000), 60000);
    CHECK(connect_window_blocks_sleep(&window, 1000000, false));
    CHECK(connect_window_blocks_sleep(&window, 61000000, true));
    CHECK(!connect_window_blocks_sleep(&window, 61000000, false));
}

static void test_stale_startup_result_ignored(void) {
    runtime_core_t core;
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    init_core(&core, false, true);

    uint32_t op = 0;
    uint32_t request = 0;
    start_to_measuring(&core, &mock, &op, &request);
    step(&core, &mock, timeout_event(request, op, MEASURE_STARTUP,
                                     3000000), 3000000);
    CHECK_INT(mock.beep_count, 1);
    CHECK_INT(mock.beeps[0].pattern, BUZZER_PATTERN_ERROR);
    uint32_t stale_request = request + 1000;
    step(&core, &mock, ina_event(true, stale_request, op, MEASURE_STARTUP,
                                 12.0f, 50.0f, 3100000), 3100000);
    CHECK_INT(mock.beep_count, 1);
}

/* ----------------------------------------------------------------- runner */

typedef void (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t fn;
} test_case_t;

static const test_case_t k_tests[] = {
    {"load_startup_sequence", test_load_startup_sequence},
    {"startup_ina_error_once", test_startup_ina_error_once},
    {"startup_voltage_selects_cue", test_startup_voltage_selects_cue},
    {"periodic_failures_silent", test_periodic_failures_silent},
    {"low_current_shutdown_failure_silent",
     test_low_current_shutdown_failure_silent},
    {"low_current_shutdown_success_silent",
     test_low_current_shutdown_success_silent},
    {"low_voltage_warning_delay", test_low_voltage_warning_delay},
    {"load_gpio_failure_handling", test_load_gpio_failure_handling},
    {"invalid_current_resets_low_timer", test_invalid_current_resets_low_timer},
    {"buzzer_disabled_completes", test_buzzer_disabled_completes},
    {"ble_lazy_start_and_deadline", test_ble_lazy_start_and_deadline},
    {"ble_disconnect_always_stops", test_ble_disconnect_always_stops},
    {"ble_stopping_press_restarts_once",
     test_ble_stopping_press_restarts_once},
    {"ble_adv_complete_stops_or_restarts",
     test_ble_adv_complete_stops_or_restarts},
    {"ble_late_callbacks_ignored", test_ble_late_callbacks_ignored},
    {"ble_pair_timeout", test_ble_pair_timeout},
    {"restored_working_sleepable", test_restored_working_sleepable},
    {"power_manager", test_power_manager},
    {"thermal_edge_and_window", test_thermal_edge_and_window},
    {"stale_startup_result_ignored", test_stale_startup_result_ignored},
};

int main(void) {
    size_t count = sizeof(k_tests) / sizeof(k_tests[0]);
    int failed_tests = 0;

    printf("Running %zu host FSM tests\n", count);
    for (size_t i = 0; i < count; ++i) {
        int before = s_failures;
        s_current_test = k_tests[i].name;
        printf("[ RUN  ] %s\n", s_current_test);
        k_tests[i].fn();
        if (s_failures == before) {
            printf("[ PASS ] %s\n", s_current_test);
        } else {
            printf("[ FAIL ] %s\n", s_current_test);
            ++failed_tests;
        }
    }

    printf("\n%zu tests, %d failed, %d check failures\n", count, failed_tests,
           s_failures);
    return failed_tests == 0 ? 0 : 1;
}
