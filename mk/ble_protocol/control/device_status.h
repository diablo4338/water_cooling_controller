#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_STATUS_VERSION 2
#define DEVICE_STATUS_PAYLOAD_LEN 6

typedef enum {
    DEVICE_STATE_OK = 0,
    DEVICE_STATE_ERROR = 1,
} device_state_t;

typedef uint32_t device_error_mask_t;

typedef enum {
    DEVICE_ERROR_NONE = 0,
    DEVICE_ERROR_ADC_OFFLINE = 1u << 0,
    DEVICE_ERROR_NTC_DISCONNECTED = 1u << 1,
} device_error_flag_t;

void device_status_init(void);
void device_status_set_error_flag(device_error_mask_t flag, bool active);
bool device_status_is_error(void);
void device_status_get_payload(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
