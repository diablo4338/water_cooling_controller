#include "pair_state.h"
#include "conn_guard.h"

static pair_state_t g_pair_state = PAIR_STATE_IDLE;

void pair_state_reset(void) {
    g_pair_state = PAIR_STATE_IDLE;
}

void pair_state_full_reset(void) {
    g_pair_state = PAIR_STATE_IDLE;
    conn_guard_reset();
}

void pair_state_start(void) {
    g_pair_state = PAIR_STATE_WAIT_HOST_PUB;
}

bool pair_state_can_host_pub(void) {
    return g_pair_state == PAIR_STATE_WAIT_HOST_PUB;
}

void pair_state_set_host_pub_ok(void) {
    g_pair_state = PAIR_STATE_WAIT_CONFIRM;
}

bool pair_state_can_confirm(void) {
    return g_pair_state == PAIR_STATE_WAIT_CONFIRM;
}

void pair_state_set_confirm_ok(void) {
    g_pair_state = PAIR_STATE_WAIT_FINISH;
}

bool pair_state_can_finish(void) {
    return g_pair_state == PAIR_STATE_WAIT_FINISH;
}

void pair_state_set_finish_ok(void) {
    g_pair_state = PAIR_STATE_FINISHED;
}

pair_state_t pair_state_get(void) {
    return g_pair_state;
}
