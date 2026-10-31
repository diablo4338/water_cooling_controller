// main/main.c
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_tasks.h"
#include "ecdh.h"
#include "gap.h"
#include "load_runtime.h"
#include "pair_mode.h"
#include "runtime_config.h"
#include "shared_i2c_bus.h"
#include "state.h"
#include "storage.h"

#ifdef PAIR_RUN_TESTS
#include "unity_test_runner.h"
void pair_tests_force_link(void);
void conn_guard_tests_force_link(void);
void metrics_tests_force_link(void);
void load_state_tests_force_link(void);
void runtime_config_tests_force_link(void);
void buzzer_tests_force_link(void);
void connect_window_tests_force_link(void);
#endif

static void create_runtime_timers(void) {
    esp_timer_create_args_t targs = {
        .callback = term_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "term"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &g_term_timer));

    esp_timer_create_args_t pargs = {
        .callback = pair_timeout_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pair_timeout"
    };
    ESP_ERROR_CHECK(esp_timer_create(&pargs, &g_pair_timer));
}

void app_main(void) {
    esp_log_level_set("pair-stack", ESP_LOG_INFO);
    esp_log_level_set("runtime", ESP_LOG_INFO);
    esp_log_level_set("runtime-config", ESP_LOG_INFO);
    esp_log_level_set("ble-lifecycle", ESP_LOG_INFO);
    esp_log_level_set("action", ESP_LOG_INFO);
    esp_log_level_set("ina226", ESP_LOG_INFO);
#ifdef PAIR_RUN_TESTS
    pair_tests_force_link();
    conn_guard_tests_force_link();
    metrics_tests_force_link();
    load_state_tests_force_link();
    runtime_config_tests_force_link();
    buzzer_tests_force_link();
    connect_window_tests_force_link();
    ESP_ERROR_CHECK(nvs_flash_init());
    unity_run_all_tests();
    return;
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    runtime_config_init();

    /* Restore held outputs before any other hardware is initialized. */
    load_runtime_init();

    ESP_ERROR_CHECK(shared_i2c_bus_init());

    fsm_reset();
    nvs_load_or_empty();
    ecdh_init();
    pair_mode_init();

    create_runtime_timers();
    gap_init();

    /* NimBLE is deliberately not initialized here.  The BLE FSM requests a
     * lazy start only after a thermal-button or GPIO19 pairing event. */
    if (xTaskCreate(load_runtime_task, "runtime", 8192, NULL, 5, NULL) !=
        pdPASS) {
        ESP_LOGE("runtime", "dispatcher task creation failed");
        abort();
    }
}
