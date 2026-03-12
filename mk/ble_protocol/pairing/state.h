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

typedef enum {
    FSM_STATE_UNPAIRED_IDLE = 0,
    FSM_STATE_PAIRING,
    FSM_STATE_PAIRED_UNAUTH,
    FSM_STATE_AUTHED
} fsm_state_t;

typedef enum {
    FSM_EVT_PAIR_START = 0,
    FSM_EVT_PAIR_TIMEOUT,
    FSM_EVT_PAIR_FINISH,
    FSM_EVT_CONNECT,
    FSM_EVT_DISCONNECT,
    FSM_EVT_AUTH_OK,
    FSM_EVT_AUTH_FAILED,
    FSM_EVT_TRUST_LOADED,
    FSM_EVT_TRUST_RESET
} fsm_event_t;
extern uint8_t g_own_addr_type;

extern uint8_t dev_nonce[16];
extern uint8_t auth_nonce[16];

extern uint8_t dev_pub65[65];
extern uint8_t host_pub65[65];

extern uint8_t K[32];
extern uint8_t host_id_hash[32];

extern esp_timer_handle_t g_term_timer;
extern esp_timer_handle_t g_pair_timer;

extern portMUX_TYPE g_state_mux;

void state_lock(void);
void state_unlock(void);

void fsm_reset(void);
bool fsm_dispatch(fsm_event_t event, uint16_t conn_handle);
fsm_state_t fsm_get_state(void);
bool fsm_is_pairing(void);
bool fsm_is_paired(void);
bool fsm_is_authed(void);

uint16_t fsm_get_conn_handle(void);
uint16_t fsm_get_term_conn_handle(void);
uint16_t fsm_get_pair_conn_handle(void);
uint16_t fsm_get_auth_conn_handle(void);

bool fsm_pair_conn_bind_or_check(uint16_t conn_handle);
bool fsm_pair_conn_check(uint16_t conn_handle);
bool fsm_auth_conn_check_or_any(uint16_t conn_handle);
bool fsm_auth_conn_check(uint16_t conn_handle);
void fsm_conn_guard_reset(void);

#endif
