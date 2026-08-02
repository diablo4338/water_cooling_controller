#include "unity.h"

#include "overheat_alarm.h"

TEST_CASE("overheat alarm sounds for a new overheat cycle", "[overheat_alarm]") {
    overheat_alarm_state_t state = {0};

    TEST_ASSERT_FALSE(overheat_alarm_update(&state, true, false, false));
    TEST_ASSERT_TRUE(overheat_alarm_update(&state, true, true, false));
}

TEST_CASE("overheat alarm silence lasts until OK then rearms", "[overheat_alarm]") {
    overheat_alarm_state_t state = {0};

    TEST_ASSERT_TRUE(overheat_alarm_update(&state, true, true, false));
    TEST_ASSERT_FALSE(overheat_alarm_update(&state, true, true, true));
    TEST_ASSERT_FALSE(overheat_alarm_update(&state, true, true, false));

    TEST_ASSERT_FALSE(overheat_alarm_update(&state, true, false, false));
    TEST_ASSERT_TRUE(overheat_alarm_update(&state, true, true, false));
}

TEST_CASE("button press in OK does not silence next alarm", "[overheat_alarm]") {
    overheat_alarm_state_t state = {0};

    TEST_ASSERT_FALSE(overheat_alarm_update(&state, true, false, true));
    TEST_ASSERT_TRUE(overheat_alarm_update(&state, true, true, false));
}

TEST_CASE("disabled overheat alarm remains off", "[overheat_alarm]") {
    overheat_alarm_state_t state = {0};

    TEST_ASSERT_FALSE(overheat_alarm_update(&state, false, true, false));
    TEST_ASSERT_TRUE(overheat_alarm_update(&state, true, true, false));
}
