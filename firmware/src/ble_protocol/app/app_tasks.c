#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "host/ble_gap.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "gap.h"
#include "pair_mode.h"
#include "params.h"
#include "overheat_alarm.h"
#include "status_led.h"
#include "state.h"
#include "storage.h"

#define FACTORY_RESET_NS "app_reset"
#define FACTORY_RESET_KEY "pending"

static struct ble_npl_event g_factory_reset_event;

static esp_err_t factory_reset_write_pending(bool pending) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FACTORY_RESET_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, FACTORY_RESET_KEY, pending ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool factory_reset_complete(void) {
    bool trust_saved = trust_reset();
    bool params_saved = params_factory_reset();
    if (!trust_saved || !params_saved) {
        ESP_LOGE(TAG, "Factory reset incomplete: trust=%s params=%s",
                 trust_saved ? "saved" : "storage error",
                 params_saved ? "saved" : "storage error");
        return false;
    }
    esp_err_t err = factory_reset_write_pending(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset completion marker failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "Factory reset completed");
    return true;
}

bool app_factory_reset_recover(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FACTORY_RESET_NS, NVS_READONLY, &handle);
    uint8_t pending = 0;
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, FACTORY_RESET_KEY, &pending);
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK || pending > 1) {
        ESP_LOGE(TAG, "Factory reset marker unreadable: value=%u, err=%s",
                 (unsigned)pending, esp_err_to_name(err));
        return false;
    }
    return pending == 0 || factory_reset_complete();
}

static void on_factory_reset(struct ble_npl_event *event) {
    (void)event;
    esp_err_t err = factory_reset_write_pending(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset start marker failed: %s", esp_err_to_name(err));
        status_led_report_factory_reset(false);
        return;
    }

    pair_mode_dispatch(PAIR_WINDOW_FACTORY_RESET);
    if (!factory_reset_complete()) {
        // Keep the marker and resume before exposing either store on the next boot.
        esp_restart();
        return;
    }
    status_led_report_factory_reset(true);
    uint16_t conn_handle = fsm_get_conn_handle();
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
    } else {
        stop_advertising();
        start_advertising();
    }
}

void app_tasks_init(void) {
    ble_npl_event_init(&g_factory_reset_event, on_factory_reset, NULL);
}

void button_task(void *p) {
    (void)p;

    int prev = 1;
    int64_t press_start = 0;
    bool pair_triggered = false;
    bool factory_triggered = false;

    while (1) {
        int v = gpio_get_level(PAIR_BTN_GPIO);

        if (prev == 1 && v == 0) {
            press_start = esp_timer_get_time();
            pair_triggered = false;
            factory_triggered = false;
            overheat_alarm_silence();
        }

        if (v == 0 && !factory_triggered) {
            int64_t dur_ms = (esp_timer_get_time() - press_start) / 1000;
            if (dur_ms > 10000) {
                factory_triggered = true;
                ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g_factory_reset_event);
            } else if (dur_ms > 3000 && !pair_triggered) {
                pair_triggered = true;
                pair_mode_request_open();
            }
        }

        prev = v;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
