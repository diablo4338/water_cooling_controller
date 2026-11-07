#include "reed_detector.h"

#include <limits.h>
#include <string.h>

static bool add_duration(int64_t now_us, uint64_t duration_us,
                         int64_t *deadline_us) {
    if (now_us < 0 || !deadline_us ||
        duration_us > (uint64_t)(INT64_MAX - now_us)) {
        return false;
    }
    *deadline_us = now_us + (int64_t)duration_us;
    return true;
}

bool reed_detector_start(reed_detector_t *detector, bool load_on,
                         uint16_t min_speed_deci_kmh,
                         uint16_t wheel_circumference_mm, int64_t now_us) {
    if (!detector || detector->attempted || load_on || now_us < 0 ||
        min_speed_deci_kmh < REED_MIN_SPEED_DECI_KMH ||
        min_speed_deci_kmh > REED_MAX_SPEED_DECI_KMH ||
        wheel_circumference_mm < REED_MIN_WHEEL_CIRCUMFERENCE_MM ||
        wheel_circumference_mm > REED_MAX_WHEEL_CIRCUMFERENCE_MM) {
        return false;
    }

    uint32_t generation = detector->generation + 1U;
    if (generation == 0) generation = 1U;
    memset(detector, 0, sizeof(*detector));
    detector->generation = generation;
    detector->attempted = true;

    /* One pulse per revolution: T_us = circumference_mm * 36000 / speed_deci_kmh. */
    const uint64_t numerator =
        (uint64_t)wheel_circumference_mm * 36000ULL;
    const uint64_t denominator = min_speed_deci_kmh;
    uint64_t interval_us = (numerator + denominator / 2ULL) / denominator;
    if (interval_us == 0 || interval_us > (uint64_t)INT64_MAX / 2ULL ||
        !add_duration(now_us, interval_us * 2ULL, &detector->deadline_us)) {
        memset(detector, 0, sizeof(*detector));
        detector->generation = generation;
        detector->attempted = true;
        return false;
    }

    detector->active = true;
    detector->max_interval_us = (int64_t)interval_us;
    return true;
}

bool reed_detector_pulse(reed_detector_t *detector, uint32_t generation,
                         int64_t timestamp_us) {
    if (!detector || !detector->active ||
        generation != detector->generation || timestamp_us < 0) {
        return false;
    }
    if (timestamp_us >= detector->deadline_us) {
        detector->active = false;
        return false;
    }
    if (!detector->waiting_for_second_pulse) {
        detector->first_pulse_us = timestamp_us;
        detector->waiting_for_second_pulse = true;
        uint64_t second_timeout =
            (uint64_t)detector->max_interval_us +
            (uint64_t)detector->max_interval_us / 2ULL;
        if (!add_duration(timestamp_us, second_timeout,
                          &detector->deadline_us)) {
            detector->active = false;
        }
        return false;
    }

    int64_t delta = timestamp_us - detector->first_pulse_us;
    detector->active = false;
    return delta >= 0 && delta <= detector->max_interval_us;
}

bool reed_detector_expire(reed_detector_t *detector, int64_t now_us) {
    if (!detector || !detector->active || now_us < detector->deadline_us) {
        return false;
    }
    detector->active = false;
    return true;
}

bool reed_detector_debounce(int64_t *last_pulse_us, int64_t timestamp_us) {
    if (!last_pulse_us || timestamp_us < 0) return false;
    if (*last_pulse_us >= 0 &&
        timestamp_us - *last_pulse_us < REED_SWITCH_DEBOUNCE_US) {
        return false;
    }
    *last_pulse_us = timestamp_us;
    return true;
}

void reed_detector_stop(reed_detector_t *detector) {
    if (detector) detector->active = false;
}
