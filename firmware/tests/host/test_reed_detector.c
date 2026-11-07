#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "reed_detector.h"

static void test_load_already_on_does_not_start(void) {
    reed_detector_t detector = {0};
    assert(!reed_detector_start(&detector, true, 50, 2100, 1000000));
    assert(!detector.active);
}

static void test_one_shot_pair_and_phase_deadlines(void) {
    reed_detector_t detector = {0};
    assert(reed_detector_start(&detector, false, 50, 2100, 1000000));
    int64_t interval = detector.max_interval_us;
    uint32_t generation = detector.generation;
    int64_t first = 1000000 + interval;

    assert(!reed_detector_pulse(&detector, generation, first));
    assert(detector.active && detector.waiting_for_second_pulse);
    assert(detector.deadline_us == first + interval + interval / 2);
    assert(reed_detector_pulse(&detector, generation, first + interval - 1));
    assert(!detector.active);
    assert(!reed_detector_pulse(&detector, generation, first + interval));
    assert(!reed_detector_start(&detector, false, 50, 2100, 0));
}

static void test_exact_maximum_interval_confirms(void) {
    reed_detector_t detector = {0};
    assert(reed_detector_start(&detector, false, 100, 2100, 0));
    int64_t first = 1000;
    assert(!reed_detector_pulse(&detector, detector.generation, first));
    assert(reed_detector_pulse(&detector, detector.generation,
                               first + detector.max_interval_us));
    assert(!detector.active);
}

static void test_late_second_pulse_is_not_motion(void) {
    reed_detector_t detector = {0};
    assert(reed_detector_start(&detector, false, 50, 2100, 0));
    int64_t interval = detector.max_interval_us;
    int64_t first = 1000;
    assert(!reed_detector_pulse(&detector, detector.generation, first));
    assert(!reed_detector_pulse(&detector, detector.generation,
                                first + interval + 1));
    assert(!detector.active);
}

static void test_timeouts_and_deadline_races(void) {
    reed_detector_t detector = {0};
    assert(reed_detector_start(&detector, false, 50, 2100, 500));
    int64_t initial_deadline = detector.deadline_us;
    assert(!reed_detector_expire(&detector, initial_deadline - 1));
    assert(reed_detector_expire(&detector, initial_deadline));
    assert(!detector.active);

    detector = (reed_detector_t){0};
    assert(reed_detector_start(&detector, false, 50, 2100, 500));
    uint32_t generation = detector.generation;
    assert(!reed_detector_pulse(&detector, generation,
                                detector.deadline_us - 1));
    int64_t second_deadline = detector.deadline_us;
    assert(!reed_detector_expire(&detector, second_deadline - 1));
    assert(reed_detector_expire(&detector, second_deadline));

    detector = (reed_detector_t){0};
    assert(reed_detector_start(&detector, false, 50, 2100, 500));
    assert(!reed_detector_pulse(&detector, detector.generation,
                                detector.deadline_us));
    assert(!detector.active);
}

static void test_debounce_rejects_bounce_but_accepts_supported_pulse(void) {
    int64_t last = -1;
    assert(reed_detector_debounce(&last, 100000));
    assert(!reed_detector_debounce(&last,
                                   100000 + REED_SWITCH_DEBOUNCE_US - 1));
    assert(reed_detector_debounce(&last,
                                  100000 + REED_SWITCH_DEBOUNCE_US));
    assert(reed_detector_start(&(reed_detector_t){0}, false, 1200, 300, 0));
}

static void test_invalid_configuration_and_stale_generation(void) {
    reed_detector_t detector = {0};
    assert(!reed_detector_start(&detector, false, 0, 2100, 0));
    assert(!reed_detector_start(&detector, false, 50, 299, 0));
    assert(reed_detector_start(&detector, false, 50, 2100, 0));
    uint32_t generation = detector.generation;
    assert(!reed_detector_start(&detector, true, 50, 2100, 1));
    assert(detector.active);
    assert(!reed_detector_pulse(&detector, generation + 1, 100));
}

int main(void) {
    test_load_already_on_does_not_start();
    test_one_shot_pair_and_phase_deadlines();
    test_exact_maximum_interval_confirms();
    test_late_second_pulse_is_not_motion();
    test_timeouts_and_deadline_races();
    test_debounce_rejects_bounce_but_accepts_supported_pulse();
    test_invalid_configuration_and_stale_generation();
    puts("reed detector tests passed");
    return 0;
}
