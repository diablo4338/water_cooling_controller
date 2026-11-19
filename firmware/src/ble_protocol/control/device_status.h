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
    DEVICE_ERROR_OVERHEAT = 1u << 2,
    DEVICE_ERROR_OVERCURRENT = 1u << 3,
    DEVICE_ERROR_INA_OFFLINE = 1u << 4,
    DEVICE_ERROR_OUTPUT_ALARM = 1u << 5,
    DEVICE_ERROR_OUTPUT_SENSOR = 1u << 6,
    DEVICE_ERROR_OUTPUT_VERIFY = 1u << 7,
    DEVICE_ERROR_FAN_OUTPUT = 1u << 8,
    DEVICE_ERROR_CONFIG_STORAGE = 1u << 9,
    DEVICE_ERROR_ALERT_WITHOUT_LOAD = 1u << 10,
} device_error_flag_t;

void device_status_init(void);
// Replace only the bits in update_mask with the corresponding bits from active_mask.
void device_status_update_error_flags(device_error_mask_t update_mask, device_error_mask_t active_mask);
void device_status_set_error_flag(device_error_mask_t flag, bool active);
bool device_status_is_error(void);
bool device_status_has_error_flag(device_error_mask_t flag);
void device_status_get_payload(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
