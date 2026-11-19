#include "pair_state.h"
#include "conn_guard.h"

typedef struct {
    pair_state_t from;
    pair_state_t to;
} pair_transition_t;

static const pair_transition_t g_transitions[] = {
    [PAIR_EVT_SESSION_STARTED] = {PAIR_STATE_IDLE, PAIR_STATE_WAIT_HOST_PUB},
    [PAIR_EVT_HOST_PUB_ACCEPTED] = {PAIR_STATE_WAIT_HOST_PUB, PAIR_STATE_WAIT_CONFIRM},
    [PAIR_EVT_CONFIRM_ACCEPTED] = {PAIR_STATE_WAIT_CONFIRM, PAIR_STATE_WAIT_FINISH},
    [PAIR_EVT_FINISH_ACCEPTED] = {PAIR_STATE_WAIT_FINISH, PAIR_STATE_FINISHED},
};

static pair_state_t g_pair_state = PAIR_STATE_IDLE;

bool pair_state_can_dispatch(pair_event_t event) {
    if (event == PAIR_EVT_SESSION_RESET) {
        return true;
    }
    return (unsigned)event < sizeof(g_transitions) / sizeof(g_transitions[0]) &&
           g_pair_state == g_transitions[event].from;
}

bool pair_state_dispatch(pair_event_t event) {
    if (!pair_state_can_dispatch(event)) {
        return false;
    }
    if (event == PAIR_EVT_SESSION_RESET) {
        g_pair_state = PAIR_STATE_IDLE;
        conn_guard_reset();
    } else {
        g_pair_state = g_transitions[event].to;
    }
    return true;
}

pair_state_t pair_state_get(void) {
    return g_pair_state;
}
