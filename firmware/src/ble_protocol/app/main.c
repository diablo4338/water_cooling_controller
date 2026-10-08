// main/main.c
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "app_tasks.h"
#include "ecdh.h"
#include "gatt.h"
#include "gap.h"
#include "state.h"
#include "storage.h"
#include "uuid.h"
#include "shared_i2c_bus.h"
#include "pair_mode.h"
#include "load_runtime.h"
#include "runtime_config.h"

#ifdef PAIR_RUN_TESTS
#include "unity_test_runner.h"
void pair_tests_force_link(void);
void conn_guard_tests_force_link(void);
void metrics_tests_force_link(void);
void load_state_tests_force_link(void);
void connect_window_tests_force_link(void);
#endif

void app_main(void) {
    esp_log_level_set("pair-stack", ESP_LOG_INFO);
    esp_log_level_set("load", ESP_LOG_INFO);
    esp_log_level_set("ina226", ESP_LOG_INFO);
    esp_log_level_set("runtime-config", ESP_LOG_INFO);
#ifdef PAIR_RUN_TESTS
    pair_tests_force_link();
    conn_guard_tests_force_link();
    metrics_tests_force_link();
    load_state_tests_force_link();
    connect_window_tests_force_link();
    ESP_ERROR_CHECK(nvs_flash_init());
    unity_run_all_tests();
    return;
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    runtime_config_init();
    load_runtime_init();
    ESP_ERROR_CHECK(shared_i2c_bus_init());
    load_runtime_cycle();
    if (load_runtime_window_remaining_ms() == 0) {
        load_runtime_sleep();
        return;
    }
    fsm_reset();
    nvs_load_or_empty();

    ecdh_init();
    pair_mode_init();

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

    gatt_init_uuids_and_services();

    ESP_ERROR_CHECK(nimble_port_init());
    gap_init();
    load_runtime_process_pair_button();
    if (!fsm_is_paired() && !pair_mode_is_active()) {
        pair_mode_activate();
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set("sensor");

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);

    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(host_task);
    xTaskCreate(load_runtime_task, "load", 4096, NULL, 5, NULL);
}
