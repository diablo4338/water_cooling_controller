#include "unity.h"

#include "access.h"
#include "pair_state.h"
#include "state.h"

void pair_tests_force_link(void) {}

static void reset_globals(void) {
    fsm_reset();
    pair_state_dispatch(PAIR_EVT_SESSION_RESET);
}

TEST_CASE("pair state rejects skipped and repeated steps", "[pair]") {
    reset_globals();

    TEST_ASSERT_EQUAL(PAIR_STATE_IDLE, pair_state_get());
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_HOST_PUB_ACCEPTED));

    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_SESSION_STARTED));
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_SESSION_STARTED));
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_CONFIRM_ACCEPTED));
    TEST_ASSERT_EQUAL(PAIR_STATE_WAIT_HOST_PUB, pair_state_get());

    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_HOST_PUB_ACCEPTED));
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_HOST_PUB_ACCEPTED));
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_FINISH_ACCEPTED));
    TEST_ASSERT_EQUAL(PAIR_STATE_WAIT_CONFIRM, pair_state_get());

    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_CONFIRM_ACCEPTED));
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_CONFIRM_ACCEPTED));
    TEST_ASSERT_EQUAL(PAIR_STATE_WAIT_FINISH, pair_state_get());

    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_FINISH_ACCEPTED));
    TEST_ASSERT_EQUAL(PAIR_STATE_FINISHED, pair_state_get());
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_FINISH_ACCEPTED));
}

TEST_CASE("pair state reset clears progress and link binding", "[pair]") {
    reset_globals();

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_CONNECT, 7));
    TEST_ASSERT_TRUE(fsm_pair_conn_bind_or_check(7));
    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_SESSION_STARTED));
    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_HOST_PUB_ACCEPTED));

    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_SESSION_RESET));
    TEST_ASSERT_EQUAL(PAIR_STATE_IDLE, pair_state_get());
    TEST_ASSERT_FALSE(pair_state_dispatch(PAIR_EVT_CONFIRM_ACCEPTED));
    TEST_ASSERT_FALSE(fsm_pair_conn_check(7));
    TEST_ASSERT_TRUE(pair_state_dispatch(PAIR_EVT_SESSION_STARTED));
}

TEST_CASE("state handles reset to none", "[state]") {
    reset_globals();
    TEST_ASSERT_EQUAL(BLE_HS_CONN_HANDLE_NONE, fsm_get_pair_conn_handle());
    TEST_ASSERT_EQUAL(BLE_HS_CONN_HANDLE_NONE, fsm_get_auth_conn_handle());
}

TEST_CASE("access control gates pairing and data", "[access]") {
    reset_globals();

    TEST_ASSERT_FALSE(can_access_pairing());
    TEST_ASSERT_FALSE(can_access_auth_nonce(7));
    TEST_ASSERT_FALSE(can_access_data(7));

    fsm_reset();
    fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
    TEST_ASSERT_FALSE(can_access_pairing());
    TEST_ASSERT_FALSE(can_access_auth_nonce(7));

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_CONNECT, 7));
    TEST_ASSERT_TRUE(can_access_auth_nonce(7));
    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_AUTH_OK, 7));
    TEST_ASSERT_TRUE(can_access_data(7));
}
