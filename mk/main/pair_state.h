#ifndef PAIR_STATE_H
#define PAIR_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAIR_STATE_IDLE = 0,
    PAIR_STATE_WAIT_HOST_PUB,
    PAIR_STATE_WAIT_CONFIRM,
    PAIR_STATE_WAIT_FINISH,
    PAIR_STATE_FINISHED
} pair_state_t;

void pair_state_reset(void);
void pair_state_full_reset(void);
void pair_state_start(void);

bool pair_state_can_host_pub(void);
void pair_state_set_host_pub_ok(void);

bool pair_state_can_confirm(void);
void pair_state_set_confirm_ok(void);

bool pair_state_can_finish(void);
void pair_state_set_finish_ok(void);

pair_state_t pair_state_get(void);

#ifdef __cplusplus
}
#endif

#endif
