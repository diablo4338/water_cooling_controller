#include "pair_state.h"
#include "conn_guard.h"
#include "esp_log.h"

static pair_state_t g_pair_state = PAIR_STATE_IDLE;

static const char *pair_state_name(pair_state_t state) {
    switch (state) {
        case PAIR_STATE_IDLE: return "IDLE";
        case PAIR_STATE_WAIT_HOST_PUB: return "WAIT_HOST_PUB";
        case PAIR_STATE_WAIT_CONFIRM: return "WAIT_CONFIRM";
        case PAIR_STATE_WAIT_FINISH: return "WAIT_FINISH";
        case PAIR_STATE_FINISHED: return "FINISHED";
        default: return "UNKNOWN";
    }
}

static void transition(pair_state_t next, const char *reason) {
    pair_state_t previous = g_pair_state;
    g_pair_state = next;
    if (previous != next) {
        ESP_LOGI("pair-stack", "PAIR %s -> %s reason=%s",
                 pair_state_name(previous), pair_state_name(next), reason);
    }
}

void pair_state_reset(void) {
    transition(PAIR_STATE_IDLE, "reset");
}

void pair_state_full_reset(void) {
    transition(PAIR_STATE_IDLE, "full_reset");
    conn_guard_reset();
}

void pair_state_start(void) {
    transition(PAIR_STATE_WAIT_HOST_PUB, "session_start");
}

bool pair_state_can_host_pub(void) {
    return g_pair_state == PAIR_STATE_WAIT_HOST_PUB;
}

void pair_state_set_host_pub_ok(void) {
    transition(PAIR_STATE_WAIT_CONFIRM, "host_pub_ok");
}

bool pair_state_can_confirm(void) {
    return g_pair_state == PAIR_STATE_WAIT_CONFIRM;
}

void pair_state_set_confirm_ok(void) {
    transition(PAIR_STATE_WAIT_FINISH, "confirm_ok");
}

bool pair_state_can_finish(void) {
    return g_pair_state == PAIR_STATE_WAIT_FINISH;
}

void pair_state_set_finish_ok(void) {
    transition(PAIR_STATE_FINISHED, "finish_ok");
}

pair_state_t pair_state_get(void) {
    return g_pair_state;
}
