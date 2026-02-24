// main/main.c
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

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

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_load_or_empty();

    ecdh_init();

    esp_timer_create_args_t targs = {
        .callback = term_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "term"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &g_term_timer));

    esp_timer_create_args_t dargs = {
        .callback = data_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "data"
    };
    ESP_ERROR_CHECK(esp_timer_create(&dargs, &g_data_timer));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PAIR_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    xTaskCreate(button_task, "btn", 4096, NULL, 5, NULL);

    gatt_init_uuids_and_services();

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set("sensor");

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);

    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(host_task);
}
