#include "pair_mode.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "crypto.h"
#include "ecdh.h"
#include "gap.h"
#include "pair_state.h"
#include "state.h"

#define PAIR_MODE_TIMEOUT_US (60ULL * 1000ULL * 1000ULL)

static bool g_pair_active = false;

static void clear_pair_session_buffers(void) {
    state_lock();
    memset(dev_nonce, 0, sizeof(dev_nonce));
    memset(dev_pub65, 0, sizeof(dev_pub65));
    memset(host_pub65, 0, sizeof(host_pub65));
    memset(K, 0, sizeof(K));
    memset(host_id_hash, 0, sizeof(host_id_hash));
    state_unlock();
}

bool pair_mode_prepare_session(void) {
    if (!g_pair_active) {
        return false;
    }

    clear_pair_session_buffers();
    rand_bytes(dev_nonce, sizeof(dev_nonce));
    if (ecdh_make_dev_keys() != 0) {
        ESP_LOGE(TAG, "ECDH keygen failed");
        clear_pair_session_buffers();
        return false;
    }
    pair_state_start();
    return true;
}

void pair_mode_init(void) {
    g_pair_active = false;
    pair_state_full_reset();
    clear_pair_session_buffers();
}

bool pair_mode_is_active(void) {
    return g_pair_active;
}

bool pair_mode_activate(void) {
    g_pair_active = true;
    pair_state_full_reset();
    if (!pair_mode_prepare_session()) {
        g_pair_active = false;
        return false;
    }

    esp_timer_stop(g_pair_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(g_pair_timer, PAIR_MODE_TIMEOUT_US));

    stop_advertising();
    start_advertising();
    ESP_LOGW(TAG, "Pairing mode ON (60s window)");
    return true;
}

void pair_mode_deactivate(void) {
    if (!g_pair_active) {
        return;
    }

    g_pair_active = false;
    esp_timer_stop(g_pair_timer);
    pair_state_full_reset();
    clear_pair_session_buffers();
    ESP_LOGW(TAG, "Pairing mode OFF");
}
