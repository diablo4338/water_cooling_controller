#ifndef UUID_H
#define UUID_H

#include "host/ble_uuid.h"

extern const char *UUID_PAIR_SVC_STR;
extern const char *UUID_MAIN_SVC_STR;
extern const char *UUID_METRICS_SVC_STR;
extern const char *UUID_CONFIG_SVC_STR;

extern const char *UUID_PAIR_DEV_NONCE_STR;
extern const char *UUID_PAIR_DEV_PUB_STR;
extern const char *UUID_PAIR_HOST_PUB_STR;
extern const char *UUID_PAIR_CONFIRM_STR;
extern const char *UUID_PAIR_FINISH_STR;

extern const char *UUID_AUTH_NONCE_STR;
extern const char *UUID_AUTH_PROOF_STR;
extern const char *UUID_CONFIG_PARAMS_STR;
extern const char *UUID_CONFIG_STATUS_STR;
extern const char *UUID_CONFIG_FAN_STATUS_STR;
extern const char *UUID_CONFIG_FAN_CALIBRATE_STR;
extern const char *UUID_TEMP0_VALUE_STR;
extern const char *UUID_TEMP1_VALUE_STR;
extern const char *UUID_TEMP2_VALUE_STR;
extern const char *UUID_TEMP3_VALUE_STR;
extern const char *UUID_FAN_SPEED_VALUE_STR;

extern ble_uuid_any_t UUID_PAIR_SVC;
extern ble_uuid_any_t UUID_MAIN_SVC;
extern ble_uuid_any_t UUID_METRICS_SVC;
extern ble_uuid_any_t UUID_CONFIG_SVC;

extern ble_uuid_any_t UUID_PAIR_DEV_NONCE;
extern ble_uuid_any_t UUID_PAIR_DEV_PUB;
extern ble_uuid_any_t UUID_PAIR_HOST_PUB;
extern ble_uuid_any_t UUID_PAIR_CONFIRM;
extern ble_uuid_any_t UUID_PAIR_FINISH;

extern ble_uuid_any_t UUID_AUTH_NONCE;
extern ble_uuid_any_t UUID_AUTH_PROOF;
extern ble_uuid_any_t UUID_CONFIG_PARAMS;
extern ble_uuid_any_t UUID_CONFIG_STATUS;
extern ble_uuid_any_t UUID_CONFIG_FAN_STATUS;
extern ble_uuid_any_t UUID_CONFIG_FAN_CALIBRATE;
extern ble_uuid_any_t UUID_TEMP0_VALUE;
extern ble_uuid_any_t UUID_TEMP1_VALUE;
extern ble_uuid_any_t UUID_TEMP2_VALUE;
extern ble_uuid_any_t UUID_TEMP3_VALUE;
extern ble_uuid_any_t UUID_FAN_SPEED_VALUE;

#endif
