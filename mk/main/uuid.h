#ifndef UUID_H
#define UUID_H

#include "host/ble_uuid.h"

extern const char *UUID_PAIR_SVC_STR;
extern const char *UUID_MAIN_SVC_STR;

extern const char *UUID_PAIR_DEV_NONCE_STR;
extern const char *UUID_PAIR_DEV_PUB_STR;
extern const char *UUID_PAIR_HOST_PUB_STR;
extern const char *UUID_PAIR_CONFIRM_STR;
extern const char *UUID_PAIR_FINISH_STR;

extern const char *UUID_AUTH_NONCE_STR;
extern const char *UUID_AUTH_PROOF_STR;

extern const char *UUID_MAIN_DATA_STR;

extern ble_uuid_any_t UUID_PAIR_SVC;
extern ble_uuid_any_t UUID_MAIN_SVC;

extern ble_uuid_any_t UUID_PAIR_DEV_NONCE;
extern ble_uuid_any_t UUID_PAIR_DEV_PUB;
extern ble_uuid_any_t UUID_PAIR_HOST_PUB;
extern ble_uuid_any_t UUID_PAIR_CONFIRM;
extern ble_uuid_any_t UUID_PAIR_FINISH;

extern ble_uuid_any_t UUID_AUTH_NONCE;
extern ble_uuid_any_t UUID_AUTH_PROOF;

extern ble_uuid_any_t UUID_MAIN_DATA;

#endif
