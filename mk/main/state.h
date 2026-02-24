#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include "host/ble_hs.h"

#define PAIR_BTN_GPIO 7

extern const char *TAG;

extern bool g_pairing_mode;
extern bool g_paired;
extern bool g_authed;

extern uint16_t g_conn_handle;
extern uint8_t g_own_addr_type;

extern uint8_t dev_nonce[16];
extern uint8_t auth_nonce[16];

extern uint8_t dev_pub65[65];
extern uint8_t host_pub65[65];

extern uint8_t K[32];
extern uint8_t host_id_hash[32];

extern esp_timer_handle_t g_term_timer;
extern esp_timer_handle_t g_data_timer;
extern esp_timer_handle_t g_pair_timer;

extern bool g_data_notify_enabled;
extern uint16_t g_data_attr_handle;

extern uint16_t g_pair_conn_handle;
extern uint16_t g_auth_conn_handle;

extern uint16_t g_term_conn_handle;

extern portMUX_TYPE g_state_mux;

void state_lock(void);
void state_unlock(void);

#endif
