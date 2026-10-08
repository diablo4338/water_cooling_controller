#include <math.h>
#include "unity.h"
#include "buzzer_logic.h"

void buzzer_tests_force_link(void) {}

static const buzzer_policy_t policy = {
    .enabled = true,
    .threshold_mv = 11000,
    .low_delay_s = 10,
    .button_ms = 500,
    .low_ms = 1000,
    .combined_ms = 1500,
};

TEST_CASE("global disable silences buzzer and resets low-voltage timer", "[buzzer]") {
    buzzer_state_t state = {0};
    buzzer_step(&state, &policy, false, 10.9f, 0);
    buzzer_policy_t disabled = policy;
    disabled.enabled = false;
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &disabled, true, 10.9f, 11000000));
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &disabled, false, 10.9f, 22000000));
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &policy, false, 10.9f, 23000000));
    TEST_ASSERT_EQUAL(1000, buzzer_step(&state, &policy, false, 10.9f, 34000000));
}

TEST_CASE("thermal press selects normal or combined buzzer duration", "[buzzer]") {
    buzzer_state_t state = {0};
    TEST_ASSERT_EQUAL(500, buzzer_step(&state, &policy, true, 12.0f, 0));
    TEST_ASSERT_EQUAL(1500, buzzer_step(&state, &policy, true, 10.9f, 1000000));
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &policy, false, 11.0f, 2000000));
}

TEST_CASE("low voltage alerts once after more than ten seconds", "[buzzer]") {
    buzzer_state_t state = {0};
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &policy, false, 10.9f, 0));
    buzzer_state_t restored = state;
    TEST_ASSERT_EQUAL(0, buzzer_step(&restored, &policy, false, 10.9f, 10000000));
    TEST_ASSERT_EQUAL(1000, buzzer_step(&restored, &policy, false, 10.9f, 11000000));
    TEST_ASSERT_EQUAL(0, buzzer_step(&restored, &policy, false, 10.9f, 20000000));
    TEST_ASSERT_EQUAL(0, buzzer_step(&restored, &policy, false, 11.0f, 21000000));
    TEST_ASSERT_EQUAL(0, buzzer_step(&restored, &policy, false, 10.9f, 22000000));
    TEST_ASSERT_EQUAL(1000, buzzer_step(&restored, &policy, false, 10.9f, 33000000));
}

TEST_CASE("invalid voltage interrupts the warning timer", "[buzzer]") {
    buzzer_state_t state = {0};
    buzzer_step(&state, &policy, false, 10.9f, 0);
    buzzer_step(&state, &policy, false, NAN, 9000000);
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &policy, false, 10.9f, 11000000));
    TEST_ASSERT_EQUAL(1000, buzzer_step(&state, &policy, false, 10.9f, 22000000));
}

TEST_CASE("combined event suppresses simultaneous low alert", "[buzzer]") {
    buzzer_state_t state = {0};
    buzzer_step(&state, &policy, false, 10.9f, 0);
    TEST_ASSERT_EQUAL(1500, buzzer_step(&state, &policy, true, 10.9f, 11000000));
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &policy, false, 10.9f, 12000000));
}

TEST_CASE("changing voltage threshold starts a new warning interval", "[buzzer]") {
    buzzer_state_t state = {0};
    buzzer_step(&state, &policy, false, 10.0f, 0);
    buzzer_policy_t changed = policy;
    changed.threshold_mv = 12000;
    changed.low_delay_s = 2;
    TEST_ASSERT_EQUAL(0, buzzer_step(&state, &changed, false, 10.0f, 11000000));
    TEST_ASSERT_EQUAL(1000, buzzer_step(&state, &changed, false, 10.0f, 14000000));
}
