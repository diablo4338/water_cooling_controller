#ifndef OPERATION_PREFLIGHT_H
#define OPERATION_PREFLIGHT_H

#include <stdbool.h>
#include <stdint.h>

#include "operation_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OP_PREFLIGHT_RESULT_CONTINUE = 0,
    OP_PREFLIGHT_RESULT_DONE = 1,
    OP_PREFLIGHT_RESULT_ERROR = 2,
} operation_preflight_result_t;

typedef struct {
    float dc_max_percent;
    float pwm_max_percent;
} operation_preflight_caps_t;

void operation_preflight_reset(void);
bool operation_preflight_start(operation_type_t type, int64_t now_us, const char **err_text);
bool operation_preflight_is_active(void);
operation_preflight_result_t operation_preflight_step(int64_t now_us, const char **err_text);
bool operation_preflight_get_caps(operation_preflight_caps_t *out);

#ifdef __cplusplus
}
#endif

#endif
