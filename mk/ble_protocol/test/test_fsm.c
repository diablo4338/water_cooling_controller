#include "unity.h"

#include "state.h"

static void reset_fsm(void) {
    fsm_reset();
}

TEST_CASE("fsm initial state and trust load", "[fsm]") {
    reset_fsm();
    TEST_ASSERT_EQUAL(FSM_STATE_UNPAIRED_IDLE, fsm_get_state());

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE));
    TEST_ASSERT_EQUAL(FSM_STATE_PAIRED_UNAUTH, fsm_get_state());
}

TEST_CASE("fsm auth flow transitions", "[fsm]") {
    reset_fsm();

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE));
    TEST_ASSERT_EQUAL(FSM_STATE_PAIRED_UNAUTH, fsm_get_state());

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_AUTH_OK, 5));
    TEST_ASSERT_EQUAL(FSM_STATE_AUTHED, fsm_get_state());

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_AUTH_FAILED, 5));
    TEST_ASSERT_EQUAL(FSM_STATE_PAIRED_UNAUTH, fsm_get_state());
}

TEST_CASE("fsm trust reset clears auth state", "[fsm]") {
    reset_fsm();

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE));
    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_AUTH_OK, 9));
    TEST_ASSERT_EQUAL(FSM_STATE_AUTHED, fsm_get_state());

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_TRUST_RESET, BLE_HS_CONN_HANDLE_NONE));
    TEST_ASSERT_EQUAL(FSM_STATE_UNPAIRED_IDLE, fsm_get_state());
    TEST_ASSERT_EQUAL(BLE_HS_CONN_HANDLE_NONE, fsm_get_auth_conn_handle());
}

TEST_CASE("fsm connect disconnect clears handle", "[fsm]") {
    reset_fsm();

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_CONNECT, 7));
    TEST_ASSERT_EQUAL(7, fsm_get_conn_handle());

    TEST_ASSERT_TRUE(fsm_dispatch(FSM_EVT_DISCONNECT, BLE_HS_CONN_HANDLE_NONE));
    TEST_ASSERT_EQUAL(BLE_HS_CONN_HANDLE_NONE, fsm_get_conn_handle());
}

TEST_CASE("fsm term handle can be set independently", "[fsm]") {
    reset_fsm();

    fsm_set_term_conn_handle(12);
    TEST_ASSERT_EQUAL(12, fsm_get_term_conn_handle());
}
