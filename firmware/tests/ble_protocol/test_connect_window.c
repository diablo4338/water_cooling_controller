#include "unity.h"
#include "connect_window.h"

void connect_window_tests_force_link(void) {}

TEST_CASE("connect window expires exactly one minute after press", "[connect-window]") {
    connect_window_t window = {0};
    TEST_ASSERT_FALSE(connect_window_blocks_sleep(&window, 0, false));
    connect_window_open(&window, 1000000);
    TEST_ASSERT_EQUAL(60000, connect_window_remaining_ms(&window, 1000000));
    TEST_ASSERT_EQUAL(1, connect_window_remaining_ms(&window, 60999999));
    TEST_ASSERT_TRUE(connect_window_blocks_sleep(&window, 60999999, false));
    TEST_ASSERT_FALSE(connect_window_blocks_sleep(&window, 61000000, false));
}

TEST_CASE("connected client blocks sleep beyond window until disconnect", "[connect-window]") {
    connect_window_t window = {0};
    connect_window_open(&window, 0);
    TEST_ASSERT_TRUE(connect_window_blocks_sleep(&window, 3600000000LL, true));
    TEST_ASSERT_FALSE(connect_window_blocks_sleep(&window, 3600000000LL, false));
    TEST_ASSERT_TRUE(connect_window_blocks_sleep(&window, 30000000, false));
    TEST_ASSERT_EQUAL(30000, connect_window_remaining_ms(&window, 30000000));
}

TEST_CASE("another press starts a new minute", "[connect-window]") {
    connect_window_t window = {0};
    connect_window_open(&window, 0);
    connect_window_open(&window, 50000000);
    TEST_ASSERT_EQUAL(50000, connect_window_remaining_ms(&window, 60000000));
    TEST_ASSERT_FALSE(connect_window_blocks_sleep(&window, 110000000, false));
}
