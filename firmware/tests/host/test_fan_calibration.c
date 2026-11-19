#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "device_status.h"
#include "fan_control.h"
#include "metrics.h"
#include "operation_preflight.h"
#include "operation_status_ble.h"
#include "output_limit.h"
#include "params.h"

static int64_t now_us;
static float requested;
static float applied;
static uint8_t applied_type;
static bool follow_request;
static unsigned saves;
static params_t saved;
static output_limit_status_t limiter;
static metrics_fan_observation_t observations[METRICS_FAN_CHANNELS];

int64_t esp_timer_get_time(void) { return now_us; }
void state_lock(void) {}
void state_unlock(void) {}
void operation_status_notify(uint16_t owner, const uint8_t *data, size_t len) {}
bool device_status_has_error_flag(device_error_mask_t flag) { return false; }
void operation_preflight_reset(void) {}
bool operation_preflight_start(operation_type_t type, int64_t now, const char **error) { return true; }
bool operation_preflight_is_active(void) { return false; }
operation_preflight_result_t operation_preflight_step(int64_t now, const char **error) {
    return OP_PREFLIGHT_RESULT_DONE;
}
bool operation_preflight_get_caps(operation_preflight_caps_t *caps) { return false; }
output_limit_status_t output_limit_get_status(void) { return limiter; }
bool output_limit_is_adjusting(output_limit_state_t state) { return state == OUTPUT_LIMIT_MEASURE; }
void output_limit_request_stop(void) {}
bool params_read(params_t *out) { *out = saved; return true; }
uint32_t params_reset_revision(void) { return 0; }
uint8_t params_apply_operation_result(const params_t *params, uint16_t mask, uint32_t revision) {
    assert(mask == PARAM_MASK_FAN_MIN_SPEED && revision == 0);
    saved = *params;
    saves++;
    return PARAM_STATUS_OK;
}
bool fan_control_override_set_output(uint8_t op, uint8_t type, float target) {
    assert(op == OP_TYPE_FAN_CALIBRATION && type == PARAM_FAN_CONTROL_PWM);
    requested = target;
    return true;
}
void fan_control_override_clear(uint8_t op) { requested = 0.0f; }
float metrics_get_fan_speed_rpm_channel(uint8_t channel) { return 900.0f; }
bool metrics_get_fan_observation(uint8_t channel, metrics_fan_observation_t *out) {
    *out = observations[channel];
    return out->completed_us != 0;
}

static void tick(void) {
    operation_manager_tick(now_us);
    if (follow_request) {
        applied = requested;
        if (requested > 0.0f) applied_type = PARAM_FAN_CONTROL_PWM;
    }
    operation_manager_check_output(now_us, applied, applied_type);
}

static void advance(int64_t duration_us) {
    const int64_t tick_us = 200000;
    assert(duration_us % tick_us == 0);
    for (int64_t elapsed = 0; elapsed < duration_us; elapsed += tick_us) {
        now_us += tick_us;
        tick();
    }
}

static void observe(uint8_t channel, float rpm) {
    const int64_t window_us = METRICS_TACH_CHANNEL_WINDOW_MS * 1000LL;
    observations[channel] = (metrics_fan_observation_t){now_us - window_us, now_us, rpm};
    tick();
}

static void start(bool second_fan) {
    now_us = 1000000;
    requested = 0.0f;
    applied = 100.0f;
    applied_type = PARAM_FAN_CONTROL_DC;
    follow_request = true;
    saves = 0;
    saved = (params_t){.fan_min_speed = 45, .fan_control_type = PARAM_FAN_CONTROL_PWM,
                       .fan2_monitoring_enabled = second_fan};
    limiter = (output_limit_status_t){.state = OUTPUT_LIMIT_NORMAL, .effective = 100, .verified_output = true};
    for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) observations[ch] = (metrics_fan_observation_t){0};
    operation_manager_init();
    assert(operation_manager_start(OP_TYPE_FAN_CALIBRATION, 1) == OP_START_OK);
    tick();
    assert(applied == 0.0f && applied_type == PARAM_FAN_CONTROL_DC);
    advance(6000000);
    assert(applied == 10.0f);
    advance(5000000);
    observe(0, NAN);
    assert(applied == 0.0f);
    advance(3000000);
    assert(applied == 15.0f);
}

static void stable_low_rpm_saves_only_after_three_windows(void) {
    start(true);
    advance(5000000);
    for (unsigned i = 0; i < 3; i++) {
        observe(0, 140.0f);
        assert(saves == 0);
        advance(800000);
        observe(1, 125.0f);
        if (i < 2) {
            assert(saves == 0);
            advance(METRICS_TACH_SCAN_US - 800000);
        }
    }
    assert(saves == 1 && saved.fan_min_speed == 15);
    assert(operation_manager_get_state() == OP_STATE_DONE);
}

static void burst_and_cached_rpm_cannot_save(void) {
    start(false);
    advance(1000000);
    observe(0, 800.0f); // Startup burst before the settled measurement window.
    advance(4000000);
    observe(0, 140.0f); // Only one good window after settling.
    advance(METRICS_TACH_SCAN_US);
    observe(0, NAN);
    advance(METRICS_TACH_SCAN_US);
    observe(0, NAN);
    advance(10000000);
    assert(saves == 0 && saved.fan_min_speed == 45);
    assert(operation_manager_get_state() == OP_STATE_IN_SERVICE);
    assert(applied == 20.0f);
    assert(operation_manager_cancel(1));
    tick();
}

static void repeated_or_skipped_windows_cannot_confirm(void) {
    start(false);
    advance(5000000);
    observe(0, 140.0f);
    advance(2 * METRICS_TACH_SCAN_US);
    observe(0, 140.0f);
    advance(METRICS_TACH_SCAN_US);
    observe(0, 140.0f);
    assert(saves == 0 && saved.fan_min_speed == 45);
    assert(operation_manager_cancel(1));
    tick();
}

static void lost_rotation_withholds_save_while_output_is_being_verified(void) {
    start(false);
    limiter.verified_output = false;
    advance(5000000);
    observe(0, 140.0f);
    advance(METRICS_TACH_SCAN_US);
    observe(0, 140.0f);
    advance(METRICS_TACH_SCAN_US);
    observe(0, 140.0f);
    assert(saves == 0);
    advance(METRICS_TACH_SCAN_US);
    limiter.verified_output = true;
    observe(0, NAN);
    assert(saves == 0 && saved.fan_min_speed == 45);
    assert(operation_manager_get_state() == OP_STATE_IN_SERVICE);
    advance(6200000);
    assert(applied == 20.0f);
    assert(operation_manager_cancel(1));
    tick();
}

static void adjustment_requires_new_windows(void) {
    start(false);
    advance(5000000);
    observe(0, 140.0f);
    advance(METRICS_TACH_SCAN_US);
    observe(0, 140.0f);
    limiter.state = OUTPUT_LIMIT_MEASURE;
    follow_request = false;
    applied = 13.0f;
    tick();
    advance(1000000);
    follow_request = true;
    limiter.state = OUTPUT_LIMIT_LIMITED;
    tick();
    advance(5000000);
    observe(0, 140.0f);
    assert(saves == 0 && saved.fan_min_speed == 45);
    assert(operation_manager_cancel(1));
    tick();
}

static void last_step_reports_the_channel_without_saving(void) {
    start(false);
    advance(300000000);
    assert(saves == 0 && saved.fan_min_speed == 45);
    assert(operation_manager_get_state() == OP_STATE_ERROR);
    uint8_t payload[OP_STATUS_PAYLOAD_LEN];
    operation_manager_get_status_payload(payload, sizeof(payload));
    const char expected[] = "fan 1 RPM unstable";
    assert(payload[3] == sizeof(expected) - 1);
    assert(memcmp(payload + 4, expected, payload[3]) == 0);
}

int main(void) {
    stable_low_rpm_saves_only_after_three_windows();
    burst_and_cached_rpm_cannot_save();
    repeated_or_skipped_windows_cannot_confirm();
    lost_rotation_withholds_save_while_output_is_being_verified();
    adjustment_requires_new_windows();
    last_step_reports_the_channel_without_saving();
    puts("Fan calibration regression tests passed");
}
