#ifndef REED_DETECTOR_H
#define REED_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#define REED_SWITCH_DEBOUNCE_US 20000LL
#define REED_MIN_SPEED_DECI_KMH 10U
#define REED_MAX_SPEED_DECI_KMH 1200U
#define REED_MIN_WHEEL_DIAMETER_MM 300U
#define REED_MAX_WHEEL_DIAMETER_MM 3000U

typedef struct {
    bool active;
    bool attempted;
    bool waiting_for_second_pulse;
    uint32_t generation;
    int64_t max_interval_us;
    int64_t deadline_us;
    int64_t first_pulse_us;
} reed_detector_t;

bool reed_detector_start(reed_detector_t *detector, bool load_on,
                         uint16_t min_speed_deci_kmh,
                         uint16_t wheel_diameter_mm, int64_t now_us);
bool reed_detector_pulse(reed_detector_t *detector, uint32_t generation,
                         int64_t timestamp_us);
bool reed_detector_expire(reed_detector_t *detector, int64_t now_us);
bool reed_detector_debounce(int64_t *last_pulse_us, int64_t timestamp_us);
void reed_detector_stop(reed_detector_t *detector);

#endif
