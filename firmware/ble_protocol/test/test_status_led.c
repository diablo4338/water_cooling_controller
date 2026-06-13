#include "unity.h"

#include "status_led.h"

TEST_CASE("status led uses pairing as highest priority", "[status_led]") {
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_ON,
        status_led_resolve_mode_for_state(true, false, false)
    );
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_ON,
        status_led_resolve_mode_for_state(true, true, false)
    );
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_ON,
        status_led_resolve_mode_for_state(true, false, true)
    );
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_ON,
        status_led_resolve_mode_for_state(true, true, true)
    );
}

TEST_CASE("status led uses overheat before internal error", "[status_led]") {
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_BLINK_FAST,
        status_led_resolve_mode_for_state(false, true, false)
    );
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_BLINK_FAST,
        status_led_resolve_mode_for_state(false, true, true)
    );
}

TEST_CASE("status led falls back to internal error then off", "[status_led]") {
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_BLINK_SLOW,
        status_led_resolve_mode_for_state(false, false, true)
    );
    TEST_ASSERT_EQUAL(
        STATUS_LED_MODE_OFF,
        status_led_resolve_mode_for_state(false, false, false)
    );
}
