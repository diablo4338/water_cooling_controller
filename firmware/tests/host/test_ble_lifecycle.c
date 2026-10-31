#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble_fsm.h"
#include "buzzer_fsm.h"
#include "connect_window.h"
#include "load_fsm.h"
#include "power_manager.h"
#include "runtime_core.h"
#include "runtime_events.h"

#define CONNECTION_TIMEOUT_US (600LL * 1000000LL)

static int s_failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__,        \
                   #condition);                                             \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

typedef struct {
    runtime_core_t core;
    unsigned start_count;
    unsigned advertise_count;
    unsigned refresh_count;
    unsigned disconnect_count;
    uint32_t advertising_duration_ms;
} test_runtime_t;

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
    .window_us = CONNECT_WINDOW_US,
};

static void init_runtime(test_runtime_t *runtime) {
    memset(runtime, 0, sizeof(*runtime));
    runtime_core_init(&runtime->core, &k_load_config, &k_buzzer_config,
                      &k_ble_config, false, false, 0, false, false, 0);
}

static runtime_event_t event(runtime_event_kind_t kind, int64_t now_us) {
    return runtime_event_make(kind, now_us);
}

static void step(test_runtime_t *runtime, runtime_event_t event,
                 int64_t now_us) {
    runtime_command_list_t commands;
    runtime_command_list_init(&commands);
    runtime_core_step(&runtime->core, &event, now_us, &commands);
    for (uint8_t i = 0; i < commands.count; ++i) {
        switch (commands.items[i].kind) {
            case RCMD_BLE_START:
                ++runtime->start_count;
                break;
            case RCMD_BLE_START_ADVERTISING:
                ++runtime->advertise_count;
                runtime->advertising_duration_ms =
                    commands.items[i].duration_ms;
                break;
            case RCMD_BLE_REFRESH_ADVERTISING:
                ++runtime->refresh_count;
                runtime->advertising_duration_ms =
                    commands.items[i].duration_ms;
                break;
            case RCMD_BLE_DISCONNECT:
                ++runtime->disconnect_count;
                break;
            default:
                break;
        }
    }
}

static void press(test_runtime_t *runtime, int64_t now_us) {
    step(runtime, event(RTEVT_THERMAL_BUTTON_PRESSED, now_us), now_us);
}

static void sync_host(test_runtime_t *runtime, int64_t now_us) {
    runtime_event_t synced = event(RTEVT_BLE_READY, now_us);
    synced.operation_id = runtime->core.ble.operation_id;
    step(runtime, synced, now_us);
}

static void connect_client(test_runtime_t *runtime, uint16_t handle,
                           int64_t now_us) {
    runtime_event_t connected = event(RTEVT_CLIENT_CONNECTED, now_us);
    connected.operation_id = runtime->core.ble.operation_id;
    connected.request_id = runtime->core.ble.adv_request_id;
    connected.conn_handle = handle;
    step(runtime, connected, now_us);
}

static bool ble_sleep_allowed(const test_runtime_t *runtime,
                              load_fsm_state_t load_state) {
    power_snapshot_t snapshot = {
        .ble_state = runtime->core.ble.state,
        .load_state = load_state,
        .buzzer_state = BUZZER_FSM_IDLE,
    };
    return power_manager_can_sleep(&snapshot);
}

static void test_boot_and_one_shot_start(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 1000000), 1000000);
    CHECK(runtime.core.ble.state == BLE_FSM_OFF);
    CHECK(runtime.start_count == 0);

    press(&runtime, 2000000);
    uint32_t first_operation = runtime.core.ble.operation_id;
    CHECK(runtime.core.ble.state == BLE_FSM_STARTING);
    CHECK(runtime.start_count == 1);
    press(&runtime, 3000000);
    CHECK(runtime.start_count == 1);
    CHECK(runtime.core.ble.operation_id == first_operation);
    CHECK(runtime.core.ble.window_deadline_us == 63000000LL);
}

static void test_sync_starts_advertising_and_press_extends_window(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    press(&runtime, 0);
    press(&runtime, 10000000LL);
    CHECK(runtime.start_count == 1);
    sync_host(&runtime, 11000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_ADVERTISING);
    CHECK(runtime.advertise_count == 1);
    CHECK(runtime.advertising_duration_ms == INT32_MAX);
    CHECK(runtime.core.ble.window_deadline_us == 70000000LL);

    press(&runtime, 20000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_ADVERTISING);
    CHECK(runtime.start_count == 1);
    CHECK(runtime.core.ble.window_deadline_us == 80000000LL);
    CHECK(runtime.refresh_count == 0);
}

static void test_repeated_press_extends_without_restarting_advertising(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    press(&runtime, 0);
    sync_host(&runtime, 1000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_ADVERTISING);
    CHECK(runtime.advertise_count == 1);
    CHECK(runtime.advertising_duration_ms == INT32_MAX);
    uint32_t advertising_request = runtime.core.ble.adv_request_id;

    press(&runtime, 20000000LL);
    press(&runtime, 30000000LL);
    CHECK(runtime.core.ble.window_deadline_us == 90000000LL);
    CHECK(runtime.start_count == 1);
    CHECK(runtime.advertise_count == 1);
    CHECK(runtime.refresh_count == 0);
    CHECK(runtime.core.ble.adv_request_id == advertising_request);

    step(&runtime, event(RTEVT_DEADLINE_CHECK, 60000000LL), 60000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_ADVERTISING);
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 89999999LL), 89999999LL);
    CHECK(runtime.core.ble.state == BLE_FSM_ADVERTISING);
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 90000000LL), 90000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);

    step(&runtime, event(RTEVT_ADV_COMPLETE, 91000000LL), 91000000LL);
    press(&runtime, 92000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
    CHECK(runtime.start_count == 1);
    CHECK(runtime.advertise_count == 1);
    CHECK(runtime.refresh_count == 0);
}

static void test_advertising_deadline_needs_no_complete_callback(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    press(&runtime, 0);
    sync_host(&runtime, 1000000LL);
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 59999999LL), 59999999LL);
    CHECK(runtime.core.ble.state == BLE_FSM_ADVERTISING);
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 60000000LL), 60000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
    CHECK(runtime.disconnect_count == 0);
    CHECK(ble_sleep_allowed(&runtime, LOAD_FSM_IDLE));
}

static void test_connected_deadline_is_hard_and_ignores_adv_expiry(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    press(&runtime, 0);
    sync_host(&runtime, 1000000LL);
    connect_client(&runtime, 7, 20000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_CONNECTED);
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 60000000LL), 60000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_CONNECTED);
    press(&runtime, 100000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_CONNECTED);
    CHECK(runtime.start_count == 1);

    step(&runtime, event(RTEVT_DEADLINE_CHECK,
                         20000000LL + CONNECTION_TIMEOUT_US - 1),
         20000000LL + CONNECTION_TIMEOUT_US - 1);
    CHECK(runtime.core.ble.state == BLE_FSM_CONNECTED);
    step(&runtime, event(RTEVT_DEADLINE_CHECK,
                         20000000LL + CONNECTION_TIMEOUT_US),
         20000000LL + CONNECTION_TIMEOUT_US);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
    CHECK(runtime.disconnect_count == 0);
    CHECK(ble_sleep_allowed(&runtime, LOAD_FSM_WORKING));
}

static void test_disconnect_completes_without_readvertising(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    press(&runtime, 0);
    sync_host(&runtime, 1000000LL);
    connect_client(&runtime, 9, 2000000LL);
    runtime_event_t disconnected = event(RTEVT_CLIENT_DISCONNECTED,
                                         3000000LL);
    disconnected.conn_handle = 9;
    step(&runtime, disconnected, 3000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
    CHECK(ble_sleep_allowed(&runtime, LOAD_FSM_IDLE));
    CHECK(runtime.advertise_count == 1);

    press(&runtime, 4000000LL);
    sync_host(&runtime, 5000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
    CHECK(runtime.start_count == 1);
    CHECK(runtime.advertise_count == 1);
}

static void test_startup_failure_and_missing_sync_are_bounded(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    press(&runtime, 0);
    press(&runtime, 59000000LL);
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 60000000LL), 60000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
    CHECK(runtime.start_count == 1);
    CHECK(ble_sleep_allowed(&runtime, LOAD_FSM_IDLE));

    init_runtime(&runtime);
    press(&runtime, 0);
    runtime_event_t failed = event(RTEVT_BLE_START_FAILED, 1000000LL);
    failed.operation_id = runtime.core.ble.operation_id;
    step(&runtime, failed, 1000000LL);
    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
}

static void test_stale_and_duplicate_events_cannot_restart_lifecycle(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    press(&runtime, 0);
    sync_host(&runtime, 1000000LL);
    uint32_t operation = runtime.core.ble.operation_id;
    uint32_t request = runtime.core.ble.adv_request_id;
    step(&runtime, event(RTEVT_BLE_READY, 2000000LL), 2000000LL);
    CHECK(runtime.advertise_count == 1);

    step(&runtime, event(RTEVT_DEADLINE_CHECK, 60000000LL), 60000000LL);
    runtime_event_t stale_sync = event(RTEVT_BLE_READY, 61000000LL);
    stale_sync.operation_id = operation;
    step(&runtime, stale_sync, 61000000LL);
    runtime_event_t stale_connection = event(RTEVT_CLIENT_CONNECTED,
                                             62000000LL);
    stale_connection.operation_id = operation;
    stale_connection.request_id = request;
    stale_connection.conn_handle = 11;
    step(&runtime, stale_connection, 62000000LL);
    runtime_event_t duplicate_disconnect = event(RTEVT_CLIENT_DISCONNECTED,
                                                 63000000LL);
    duplicate_disconnect.conn_handle = 11;
    step(&runtime, duplicate_disconnect, 63000000LL);
    press(&runtime, 64000000LL);

    CHECK(runtime.core.ble.state == BLE_FSM_READY_TO_SLEEP);
    CHECK(runtime.start_count == 1);
    CHECK(runtime.advertise_count == 1);
    CHECK(runtime.disconnect_count == 0);
}

static void test_power_manager_ble_eligibility_and_working_load(void) {
    test_runtime_t runtime;
    init_runtime(&runtime);
    CHECK(ble_sleep_allowed(&runtime, LOAD_FSM_WORKING));
    press(&runtime, 0);
    CHECK(!ble_sleep_allowed(&runtime, LOAD_FSM_IDLE));
    sync_host(&runtime, 1000000LL);
    CHECK(!ble_sleep_allowed(&runtime, LOAD_FSM_IDLE));
    step(&runtime, event(RTEVT_DEADLINE_CHECK, 60000000LL), 60000000LL);
    CHECK(ble_sleep_allowed(&runtime, LOAD_FSM_WORKING));
}

typedef void (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t run;
} test_case_t;

int main(void) {
    const test_case_t tests[] = {
        {"boot_and_one_shot_start", test_boot_and_one_shot_start},
        {"sync_and_advertising_extension",
         test_sync_starts_advertising_and_press_extends_window},
        {"repeated_press_without_advertising_restart",
         test_repeated_press_extends_without_restarting_advertising},
        {"advertising_deadline_without_callback",
         test_advertising_deadline_needs_no_complete_callback},
        {"hard_connected_deadline",
         test_connected_deadline_is_hard_and_ignores_adv_expiry},
        {"disconnect_without_restart",
         test_disconnect_completes_without_readvertising},
        {"bounded_startup_failure",
         test_startup_failure_and_missing_sync_are_bounded},
        {"stale_and_duplicate_events",
         test_stale_and_duplicate_events_cannot_restart_lifecycle},
        {"power_manager_eligibility",
         test_power_manager_ble_eligibility_and_working_load},
    };
    const size_t count = sizeof(tests) / sizeof(tests[0]);
    unsigned failed_tests = 0;
    for (size_t i = 0; i < count; ++i) {
        int failures_before = s_failures;
        printf("[ RUN  ] %s\n", tests[i].name);
        tests[i].run();
        if (s_failures == failures_before) {
            printf("[ PASS ] %s\n", tests[i].name);
        } else {
            printf("[ FAIL ] %s\n", tests[i].name);
            ++failed_tests;
        }
    }
    printf("%zu BLE lifecycle tests, %u failed, %d assertion failures\n",
           count, failed_tests, s_failures);
    return failed_tests == 0 ? 0 : 1;
}
