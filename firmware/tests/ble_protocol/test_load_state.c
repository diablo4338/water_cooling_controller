#include <math.h>
#include "unity.h"
#include "load_state.h"

void load_state_tests_force_link(void) {}

static const load_policy_t policy = {
    .button_delta_c = 2.0f,
    .idle_current_ma = 100.0f,
    .idle_timeout_us = 300000000LL,
    .require_new_press = true,
};

static void fill_history(load_state_t *state, float temperature) {
    for (int i = 0; i < 5; ++i) {
        load_state_step(state, &policy, temperature, 200, i * 1000000LL);
    }
}

TEST_CASE("thermal button uses median of five previous samples", "[load]") {
    load_state_t state = {0};
    for (int i = 0; i < 4; ++i) load_state_step(&state, &policy, 25, 200, i * 1000000LL);
    load_state_step(&state, &policy, 27, 200, 4000000);
    TEST_ASSERT_EQUAL(LOAD_IDLE, state.mode);
    TEST_ASSERT_FALSE(state.button_was_pressed);
    load_state_step(&state, &policy, 26.9f, 200, 5000000);
    TEST_ASSERT_FALSE(state.button_was_pressed);
    load_state_step(&state, &policy, 27.0f, 200, 6000000);
    TEST_ASSERT_TRUE(state.button_was_pressed);
    TEST_ASSERT_EQUAL(LOAD_WORKING, state.mode);
}

TEST_CASE("single outlier does not move median", "[load]") {
    load_state_t state = {0};
    fill_history(&state, 25);
    load_state_step(&state, &policy, 60, 200, 5000000);
    load_state_step(&state, &policy, 25, 200, 6000000);
    TEST_ASSERT_TRUE(load_button_is_pressed(&state, &policy, 27));
}

TEST_CASE("thermal history is rolling and ignores invalid samples", "[load]") {
    load_state_t state = {0};
    fill_history(&state, 25);
    load_state_step(&state, &policy, NAN, 200, 5000000);
    TEST_ASSERT_EQUAL(5, state.temperature_count);
    TEST_ASSERT_EQUAL(0, state.temperature_next);
    load_state_step(&state, &policy, 27, 200, 6000000);
    TEST_ASSERT_TRUE(state.button_was_pressed);
    load_state_step(&state, &policy, NAN, 200, 7000000);
    TEST_ASSERT_TRUE(state.button_was_pressed);
    load_state_step(&state, &policy, 25, 200, 8000000);
    TEST_ASSERT_FALSE(state.button_was_pressed);
    load_state_step(&state, &policy, 40, 200, 9000000);
    load_state_step(&state, &policy, 40, 200, 10000000);
    load_state_step(&state, &policy, 40, 200, 11000000);
    TEST_ASSERT_FALSE(load_button_is_pressed(&state, &policy, 41));
}

TEST_CASE("retained history survives wakeup and restart needs new press", "[load]") {
    load_state_t state = {0};
    fill_history(&state, 25);
    load_state_t restored = state;
    load_state_step(&restored, &policy, 27, 200, 5000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, restored.mode);
    load_state_step(&restored, &policy, 27, 20, 6000000);
    load_state_step(&restored, &policy, 27, 20, 306000000);
    TEST_ASSERT_EQUAL(LOAD_IDLE, restored.mode);
    load_state_step(&restored, &policy, NAN, 20, 307000000);
    TEST_ASSERT_EQUAL(LOAD_IDLE, restored.mode);
    load_state_step(&restored, &policy, 25, 200, 308000000);
    load_state_step(&restored, &policy, 29, 200, 309000000);
    TEST_ASSERT_EQUAL(LOAD_WORKING, restored.mode);
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
}
