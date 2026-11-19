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

typedef enum {
    PAIR_EVT_SESSION_STARTED,
    PAIR_EVT_HOST_PUB_ACCEPTED,
    PAIR_EVT_CONFIRM_ACCEPTED,
    PAIR_EVT_FINISH_ACCEPTED,
    PAIR_EVT_SESSION_RESET,
} pair_event_t;

// Called on the BLE host task after initialization.
bool pair_state_can_dispatch(pair_event_t event);
bool pair_state_dispatch(pair_event_t event);

pair_state_t pair_state_get(void);

#ifdef __cplusplus
}
#endif

#endif
