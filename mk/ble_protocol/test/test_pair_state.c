#include "unity.h"

#include "access.h"
#include "pair_state.h"
#include "state.h"

void pair_tests_force_link(void) {}

static void reset_globals(void) {
    fsm_reset();
    pair_state_reset();
}

TEST_CASE("pair state enforces order", "[pair]") {
    reset_globals();

    TEST_ASSERT_EQUAL(PAIR_STATE_IDLE, pair_state_get());
    TEST_ASSERT_FALSE(pair_state_can_host_pub());
    TEST_ASSERT_FALSE(pair_state_can_confirm());
    TEST_ASSERT_FALSE(pair_state_can_finish());

    pair_state_start();
    TEST_ASSERT_TRUE(pair_state_can_host_pub());
    TEST_ASSERT_FALSE(pair_state_can_confirm());
    TEST_ASSERT_FALSE(pair_state_can_finish());

    pair_state_set_host_pub_ok();
    TEST_ASSERT_FALSE(pair_state_can_host_pub());
    TEST_ASSERT_TRUE(pair_state_can_confirm());
    TEST_ASSERT_FALSE(pair_state_can_finish());

    pair_state_set_confirm_ok();
    TEST_ASSERT_FALSE(pair_state_can_confirm());
    TEST_ASSERT_TRUE(pair_state_can_finish());

    pair_state_set_finish_ok();
    TEST_ASSERT_EQUAL(PAIR_STATE_FINISHED, pair_state_get());
    TEST_ASSERT_FALSE(pair_state_can_finish());
}

TEST_CASE("pair state reset clears progress", "[pair]") {
    reset_globals();

    pair_state_start();
    pair_state_set_host_pub_ok();
    pair_state_set_confirm_ok();

    pair_state_reset();
    TEST_ASSERT_EQUAL(PAIR_STATE_IDLE, pair_state_get());
    TEST_ASSERT_FALSE(pair_state_can_host_pub());
    TEST_ASSERT_FALSE(pair_state_can_confirm());
    TEST_ASSERT_FALSE(pair_state_can_finish());
}

TEST_CASE("state handles reset to none", "[state]") {
    reset_globals();
    TEST_ASSERT_EQUAL(BLE_HS_CONN_HANDLE_NONE, fsm_get_pair_conn_handle());
    TEST_ASSERT_EQUAL(BLE_HS_CONN_HANDLE_NONE, fsm_get_auth_conn_handle());
}

TEST_CASE("access control gates pairing and data", "[access]") {
    reset_globals();

    TEST_ASSERT_FALSE(can_access_pairing());
    TEST_ASSERT_FALSE(can_access_auth_nonce());
    TEST_ASSERT_FALSE(can_access_data());

    fsm_dispatch(FSM_EVT_PAIR_START, BLE_HS_CONN_HANDLE_NONE);
    TEST_ASSERT_TRUE(can_access_pairing());

    fsm_reset();
    fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
    TEST_ASSERT_FALSE(can_access_pairing());
    TEST_ASSERT_TRUE(can_access_auth_nonce());

    fsm_dispatch(FSM_EVT_AUTH_OK, 7);
    TEST_ASSERT_TRUE(can_access_data());
}
