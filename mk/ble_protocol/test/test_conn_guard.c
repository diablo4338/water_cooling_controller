#include "unity.h"
#include <string.h>

#include "conn_guard.h"
#include "state.h"
#include "host_verify.h"
#include "crypto.h"

void conn_guard_tests_force_link(void) {}

TEST_CASE("pairing conn guard binds first handle", "[conn_guard]") {
    fsm_reset();
    conn_guard_reset();
    TEST_ASSERT_TRUE(pairing_conn_bind_or_check(1));
    TEST_ASSERT_TRUE(pairing_conn_check(1));
    TEST_ASSERT_FALSE(pairing_conn_check(2));
}

TEST_CASE("auth conn guard allows any before bind", "[conn_guard]") {
    fsm_reset();
    conn_guard_reset();
    TEST_ASSERT_TRUE(auth_conn_check_or_any(3));
    TEST_ASSERT_TRUE(auth_conn_check_or_any(4));
}

TEST_CASE("auth conn guard binds after set", "[conn_guard]") {
    fsm_reset();
    conn_guard_reset();
    fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
    fsm_dispatch(FSM_EVT_AUTH_OK, 7);

    TEST_ASSERT_TRUE(auth_conn_check_or_any(7));
    TEST_ASSERT_TRUE(auth_conn_check(7));
    TEST_ASSERT_FALSE(auth_conn_check_or_any(8));
    TEST_ASSERT_FALSE(auth_conn_check(8));
}

TEST_CASE("pairing guard rejects other handle after bind", "[conn_guard]") {
    fsm_reset();
    conn_guard_reset();
    TEST_ASSERT_TRUE(pairing_conn_bind_or_check(11));
    TEST_ASSERT_FALSE(pairing_conn_check(12));
}

TEST_CASE("auth guard rejects other handle after bind", "[conn_guard]") {
    fsm_reset();
    conn_guard_reset();
    fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
    fsm_dispatch(FSM_EVT_AUTH_OK, 21);

    TEST_ASSERT_FALSE(auth_conn_check_or_any(22));
    TEST_ASSERT_FALSE(auth_conn_check(22));
}

TEST_CASE("host verify matches stored hash", "[host_verify]") {
    uint8_t pub[65];
    pub[0] = 0x04;
    for (int i = 1; i < 65; i++) pub[i] = (uint8_t)i;

    state_lock();
    memcpy(host_pub65, pub, sizeof(pub));
    state_unlock();
    host_verify_update(pub);
    TEST_ASSERT_TRUE(host_verify_check());

    state_lock();
    host_pub65[1] ^= 0xFF;
    state_unlock();
    TEST_ASSERT_FALSE(host_verify_check());
}
