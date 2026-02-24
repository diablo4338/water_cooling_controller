#include "state.h"

const char *TAG = "pair-stack";

bool g_pairing_mode = false;
bool g_paired = false;
bool g_authed = false;

uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
uint8_t g_own_addr_type;

uint8_t dev_nonce[16];
uint8_t auth_nonce[16];

uint8_t dev_pub65[65];
uint8_t host_pub65[65];

uint8_t K[32];
uint8_t host_id_hash[32];

esp_timer_handle_t g_term_timer;
esp_timer_handle_t g_data_timer;
esp_timer_handle_t g_pair_timer;

bool g_data_notify_enabled = false;
uint16_t g_data_attr_handle = 0;

uint16_t g_term_conn_handle = BLE_HS_CONN_HANDLE_NONE;
uint16_t g_pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
uint16_t g_auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;

portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

void state_lock(void) {
    portENTER_CRITICAL(&g_state_mux);
}

void state_unlock(void) {
    portEXIT_CRITICAL(&g_state_mux);
}
