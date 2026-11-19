#ifndef OPERATION_MANAGER_H
#define OPERATION_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OP_CONTROL_VERSION 1
#define OP_STATUS_VERSION 1
#define OP_CONTROL_START 1
#define OP_CONTROL_CANCEL 2
#define OP_ERROR_TEXT_MAX 20
#define OP_STATUS_PAYLOAD_LEN (4 + OP_ERROR_TEXT_MAX)

typedef enum {
    OP_TYPE_NONE = 0,
    OP_TYPE_FAN_CALIBRATION = 1,
    OP_TYPE_SETUP_FANS = 2,
} operation_type_t;

typedef enum {
    OP_STATE_IDLE = 0,
    OP_STATE_IN_SERVICE = 1,
    OP_STATE_DONE = 2,
    OP_STATE_ERROR = 3,
} operation_state_t;

typedef enum {
    OP_START_OK = 0,
    OP_START_BUSY = 1,
    OP_START_INVALID = 2,
    OP_START_OVERHEAT = 3,
} operation_start_result_t;

void operation_manager_init(void);
operation_state_t operation_manager_get_state(void);
operation_type_t operation_manager_get_active_type(void);
bool operation_manager_is_busy(void);

void operation_manager_get_status_payload(uint8_t *out, size_t len);

operation_start_result_t operation_manager_start(operation_type_t type, uint16_t owner);
bool operation_manager_cancel(uint16_t owner);
void operation_manager_on_disconnect(uint16_t owner);
void operation_manager_tick(int64_t now_us);
void operation_manager_check_output(int64_t now_us, float applied, uint8_t control_type);

#ifdef __cplusplus
}
#endif

#endif
