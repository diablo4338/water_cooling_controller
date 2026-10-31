#include "pair_mode.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "crypto.h"
#include "ecdh.h"
#include "pair_state.h"
#include "state.h"

#define PAIR_MODE_TIMEOUT_US (60ULL * 1000ULL * 1000ULL)

static bool g_pair_active = false;
static portMUX_TYPE g_pair_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_pair_deadline_ms;

uint32_t pair_mode_deadline_ms(void) {
    portENTER_CRITICAL(&g_pair_mux);
    uint32_t deadline_ms = g_pair_deadline_ms;
    portEXIT_CRITICAL(&g_pair_mux);
    return deadline_ms;
}

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
    portENTER_CRITICAL(&g_pair_mux);
    g_pair_deadline_ms = 0;
    portEXIT_CRITICAL(&g_pair_mux);
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

    if (esp_timer_stop(g_pair_timer) == ESP_OK) {
        ESP_LOGI(TAG, "Timer pair_timeout cancelled reason=restart");
    }
    uint32_t deadline_ms = (uint32_t)((esp_timer_get_time() +
                                       PAIR_MODE_TIMEOUT_US) / 1000ULL);
    portENTER_CRITICAL(&g_pair_mux);
    g_pair_deadline_ms = deadline_ms;
    portEXIT_CRITICAL(&g_pair_mux);

    esp_err_t err = esp_timer_start_once(g_pair_timer, PAIR_MODE_TIMEOUT_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pair_timeout timer start failed: %s",
                 esp_err_to_name(err));
        portENTER_CRITICAL(&g_pair_mux);
        g_pair_deadline_ms = 0;
        portEXIT_CRITICAL(&g_pair_mux);
        g_pair_active = false;
        return false;
    }
    ESP_LOGI(TAG, "Timer pair_timeout started duration_ms=60000");
    ESP_LOGW(TAG, "Pairing mode ON (60s window)");
    return true;
}

void pair_mode_deactivate(void) {
    if (!g_pair_active) {
        return;
    }

    g_pair_active = false;
    portENTER_CRITICAL(&g_pair_mux);
    g_pair_deadline_ms = 0;
    portEXIT_CRITICAL(&g_pair_mux);
    if (esp_timer_stop(g_pair_timer) == ESP_OK) {
        ESP_LOGI(TAG, "Timer pair_timeout cancelled reason=pair_mode_deactivated");
    }
    pair_state_full_reset();
    clear_pair_session_buffers();
    ESP_LOGW(TAG, "Pairing mode OFF");
}
