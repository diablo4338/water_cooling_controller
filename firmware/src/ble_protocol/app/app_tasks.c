#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_gap.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "gap.h"
#include "pair_mode.h"
#include "pair_state.h"
#include "state.h"

void pair_timeout_cb(void *arg) {
    (void)arg;
    if (!pair_mode_is_active()) {
        return;
    }
    pair_mode_deactivate();

    uint16_t conn_handle = fsm_get_conn_handle();
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE && !fsm_is_authed()) {
        int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
        return;
    }

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        stop_advertising();
        start_advertising();
    }

    ESP_LOGW(TAG, "Pairing mode OFF (timeout)");
}

void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
