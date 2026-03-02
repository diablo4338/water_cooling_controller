#include "pair_mode.h"

#include "esp_log.h"

#include "crypto.h"
#include "ecdh.h"
#include "gap.h"
#include "pair_state.h"
#include "state.h"
#include "storage.h"

static bool g_pair_forced = false;

static bool pair_session_init(void) {
    rand_bytes(dev_nonce, sizeof(dev_nonce));
    if (ecdh_make_dev_keys() != 0) {
        ESP_LOGE(TAG, "ECDH keygen failed");
        return false;
    }
    pair_state_start();
    return true;
}

void pair_mode_init_from_nvs(void) {
    if (!g_pair_forced && nvs_is_pair_forced()) {
        g_pair_forced = true;
        if (!fsm_dispatch(FSM_EVT_PAIR_START, BLE_HS_CONN_HANDLE_NONE)) {
            ESP_LOGW(TAG, "Forced pairing ignored: bad state");
            return;
        }
        if (!pair_session_init()) {
            return;
        }
        ESP_LOGW(TAG, "Forced pairing mode ON (boot)");
    }
}

bool pair_mode_is_forced(void) {
    return g_pair_forced;
}

void pair_mode_force_on(void) {
    if (!g_pair_forced) {
        g_pair_forced = true;
        nvs_set_pair_forced(true);
    }
    if (!fsm_dispatch(FSM_EVT_PAIR_START, BLE_HS_CONN_HANDLE_NONE)) {
        ESP_LOGW(TAG, "Forced pairing ignored: bad state");
        return;
    }
    if (!pair_session_init()) {
        return;
    }
    stop_advertising();
    start_advertising();
    ESP_LOGW(TAG, "Forced pairing mode ON");
}

void pair_mode_clear_on_success(uint16_t conn_handle) {
    if (!g_pair_forced) return;
    g_pair_forced = false;
    nvs_set_pair_forced(false);
    fsm_dispatch(FSM_EVT_PAIR_FINISH, conn_handle);
    ESP_LOGW(TAG, "Forced pairing mode OFF (paired)");
}
