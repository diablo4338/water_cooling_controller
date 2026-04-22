#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_STATUS_VERSION 1
#define DEVICE_STATUS_PAYLOAD_LEN 3

typedef enum {
    DEVICE_STATE_OK = 0,
    DEVICE_STATE_ERROR = 1,
} device_state_t;

typedef enum {
    DEVICE_ERROR_NONE = 0,
    DEVICE_ERROR_ADC_OFFLINE = 1,
} device_error_t;

void device_status_init(void);
void device_status_set_error(device_error_t error);
void device_status_get_payload(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
