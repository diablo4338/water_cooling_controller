
#ifndef UUID_H
#define UUID_H

#include "host/ble_uuid.h"

extern const char *UUID_PAIR_SVC_STR;
extern const char *UUID_MAIN_SVC_STR;
extern const char *UUID_METRICS_SVC_STR;

extern const char *UUID_PAIR_DEV_NONCE_STR;
extern const char *UUID_PAIR_DEV_PUB_STR;
extern const char *UUID_PAIR_HOST_PUB_STR;
extern const char *UUID_PAIR_CONFIRM_STR;
extern const char *UUID_PAIR_FINISH_STR;

extern const char *UUID_AUTH_NONCE_STR;
extern const char *UUID_AUTH_PROOF_STR;
extern const char *UUID_RUNTIME_CONFIG_STR;
extern const char *UUID_CONFIG_RESULT_STR;
extern const char *UUID_LOAD_STATE_STR;
extern const char *UUID_BUTTON_STATE_STR;
extern const char *UUID_TEMPERATURE_STR;
extern const char *UUID_VOLTAGE_V_STR;
extern const char *UUID_CURRENT_MA_STR;

extern ble_uuid_any_t UUID_PAIR_SVC;
extern ble_uuid_any_t UUID_MAIN_SVC;
extern ble_uuid_any_t UUID_METRICS_SVC;

extern ble_uuid_any_t UUID_PAIR_DEV_NONCE;
extern ble_uuid_any_t UUID_PAIR_DEV_PUB;
extern ble_uuid_any_t UUID_PAIR_HOST_PUB;
extern ble_uuid_any_t UUID_PAIR_CONFIRM;
extern ble_uuid_any_t UUID_PAIR_FINISH;

extern ble_uuid_any_t UUID_AUTH_NONCE;
extern ble_uuid_any_t UUID_AUTH_PROOF;
extern ble_uuid_any_t UUID_RUNTIME_CONFIG;
extern ble_uuid_any_t UUID_CONFIG_RESULT;
extern ble_uuid_any_t UUID_LOAD_STATE;
extern ble_uuid_any_t UUID_BUTTON_STATE;
extern ble_uuid_any_t UUID_TEMPERATURE;
extern ble_uuid_any_t UUID_VOLTAGE_V;
extern ble_uuid_any_t UUID_CURRENT_MA;

#endif
