#include <math.h>
#include "unity.h"
#include "load_state.h"

void load_state_tests_force_link(void) {}

static const load_policy_t policy = {30.0f, 27.0f, 100.0f, 300000000LL, true};

TEST_CASE("load starts above threshold and ignores working presses", "[load]") {
    load_state_t state = {0};
    load_state_step(&state, &policy, 30.0f, 0, 0);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
    load_state_step(&state, &policy, 31.0f, 0, 1000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
    TEST_ASSERT_FALSE(state.low_current_pending);
    load_state_step(&state, &policy, 20.0f, 200, 2000000);
    load_state_step(&state, &policy, 31.0f, 200, 3000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
}

TEST_CASE("load timeout spans wakeups and requires release to restart", "[load]") {
    load_state_t state = {.mode = LOAD_WORKING, .button_was_pressed = true};
    load_state_step(&state, &policy, 31, 20, 1000000);
    load_state_t restored = state;
    load_state_step(&restored, &policy, 31, 20, 300999999);
    TEST_ASSERT_EQUAL(LOAD_WORKING, restored.mode);
    load_state_step(&restored, &policy, 31, 20, 301000000);
    TEST_ASSERT_EQUAL(LOAD_IDLE, restored.mode);
    load_state_step(&restored, &policy, NAN, 20, 302000000);
    load_state_step(&restored, &policy, 31, 20, 303000000);
    TEST_ASSERT_EQUAL(LOAD_IDLE, restored.mode);
    load_state_step(&restored, &policy, 27, 20, 304000000);
    load_state_step(&restored, &policy, 31, 20, 305000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, restored.mode);
}

TEST_CASE("button hysteresis prevents repeated press above release threshold", "[load]") {
    load_state_t state = {0};
    load_state_step(&state, &policy, 30, 200, 0);
    TEST_ASSERT_TRUE(state.button_was_pressed);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
    load_state_step(&state, &policy, 28, 200, 1000000);
    TEST_ASSERT_TRUE(state.button_was_pressed);
    load_state_step(&state, &policy, 30, 200, 2000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
    load_state_step(&state, &policy, 27, 200, 3000000);
    TEST_ASSERT_FALSE(state.button_was_pressed);
    load_state_step(&state, &policy, 30, 200, 4000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
}

TEST_CASE("invalid or high INA sample resets low current timeout", "[load]") {
    load_state_t state = {.mode = LOAD_WORKING};
    load_state_step(&state, &policy, NAN, 0, 0);
    load_state_step(&state, &policy, NAN, NAN, 299000000);
    TEST_ASSERT_FALSE(state.low_current_pending);
    load_state_step(&state, &policy, NAN, 0, 300000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
    load_state_step(&state, &policy, NAN, -100, 599000000);
    TEST_ASSERT_FALSE(state.low_current_pending);
    load_state_step(&state, &policy, NAN, 0, 600000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
}
