#include <assert.h>
#include <stdio.h>

#include "device_status.h"
#include "ina226.h"
#include "output_limit.h"
#include "params.h"

static ina226_snapshot_t snapshot;
static float actual;
static uint8_t maximum = 100;
static float base = 50;
static output_limit_mode_t mode = OUTPUT_LIMIT_MODE_CONTINUOUS;
static device_error_mask_t errors;

void state_lock(void) {}
void state_unlock(void) {}
void device_status_update_error_flags(device_error_mask_t owned, device_error_mask_t active) { errors = active; }
ina226_snapshot_t ina226_get_snapshot(void) { return snapshot; }

params_config_t params_read_config(void) {
    return (params_config_t){.maximum = maximum, .params.fan_min_speed = (int32_t)base};
}

static void sample(int64_t start_ms, int64_t read_ms, float current) {
    snapshot = (ina226_snapshot_t){
        .state = INA226_READY,
        .sample_valid = true,
        .current_ma = current,
        .conversion_started_after_us = start_ms * 1000,
        .sampled_us = read_ms * 1000,
        .sequence = snapshot.sequence + 1,
    };
}

static void expect_step(int64_t ms, output_limit_state_t state, float percent) {
    output_limit_command_t command = output_limit_step(base, actual, 1, maximum, base, mode, ms * 1000);
    output_limit_state_t observed = output_limit_get_status().state;
    if (observed != state || command.percent != percent) {
        fprintf(stderr, "%lld ms: expected state %d at %.0f%%, got %d at %.0f%%\n",
                (long long)ms, state, percent, observed, command.percent);
    }
    assert(command.control_type == 1);
    assert(command.percent == percent);
    assert(observed == state);
}

static void applied(int64_t ms, float percent) {
    actual = percent;
    output_limit_output_applied(percent, 1, ms * 1000);
}

static void reset(int64_t ms, float percent) {
    sample(ms - 3000, ms, 0.0f);
    output_limit_state_t state = output_limit_get_status().state;
    expect_step(ms, state, actual);
    assert(output_limit_request_reset());
    expect_step(ms + 10, OUTPUT_LIMIT_NORMAL, percent);
    assert(output_limit_get_status().reset_result == OUTPUT_LIMIT_RESET_APPLIED);
}

int main(void) {
    const float threshold = INA226_OVERCURRENT_THRESHOLD_MA;
    base = 100;
    expect_step(100, OUTPUT_LIMIT_NORMAL, 100);
    applied(100, 100);
    sample(0, 2900, 119.2f);
    expect_step(2900, OUTPUT_LIMIT_NORMAL, 100);
    sample(2900, 5700, 119.2f);
    expect_step(5700, OUTPUT_LIMIT_VERIFY_FORMULA, 83);
    assert(output_limit_settings_locked());
    assert(output_limit_get_status().confirmed_effective == 100);
    expect_step(5800, OUTPUT_LIMIT_VERIFY_FORMULA, 83);
    applied(5900, 83);
    sample(5700, 8750, 119.2f);
    expect_step(8750, OUTPUT_LIMIT_VERIFY_FORMULA, 83);
    sample(8750, 11500, threshold);
    expect_step(11500, OUTPUT_LIMIT_SEARCH_UP, 88);
    applied(11510, 88);
    expect_step(11600, OUTPUT_LIMIT_SEARCH_UP, 88);
    sample(11600, 14500, threshold - 1);
    expect_step(14500, OUTPUT_LIMIT_SEARCH_UP, 93);
    applied(14510, 93);
    sample(14600, 17500, 119.2f);
    expect_step(17500, OUTPUT_LIMIT_VERIFY_STEP, 88);
    applied(17600, 88);
    sample(17500, 20500, 119.2f);
    expect_step(20500, OUTPUT_LIMIT_VERIFY_STEP, 88);
    sample(20600, 23500, 99.0f);
    expect_step(23500, OUTPUT_LIMIT_LIMITED, 88);
    assert(output_limit_get_status().verified_output && errors == DEVICE_ERROR_NONE);

    // Above threshold in LIMITED must start a new search even without a pin event.
    sample(23500, 26500, 119.2f);
    expect_step(26500, OUTPUT_LIMIT_VERIFY_FORMULA, 73);
    assert(output_limit_get_status().confirmed_effective == 88);
    applied(26600, 73);
    sample(26700, 29500, 119.2f);
    expect_step(29500, OUTPUT_LIMIT_VERIFY_STEP, 68);
    applied(29600, 68);
    sample(29700, 32500, 119.2f);
    expect_step(32500, OUTPUT_LIMIT_FAULT, 0);
    assert(errors & DEVICE_ERROR_OUTPUT_ALARM);
    applied(32600, 0);
    assert(!output_limit_request_reset());
    sample(32500, 35500, 119.2f);
    expect_step(35500, OUTPUT_LIMIT_FAULT, 0);
    reset(39000, 100);

    // Zero-output verification must also ignore a window spanning the output change.
    base = 3;
    applied(39100, 3);
    sample(39200, 42000, threshold * 5);
    expect_step(42000, OUTPUT_LIMIT_VERIFY_FORMULA, 0);
    applied(42100, 0);
    sample(42000, 45000, 119.2f);
    expect_step(45000, OUTPUT_LIMIT_VERIFY_FORMULA, 0);
    sample(45000, 48000, 119.2f);
    expect_step(48000, OUTPUT_LIMIT_FAULT, 0);
    assert(output_limit_get_status().alert_without_load && (errors & DEVICE_ERROR_ALERT_WITHOUT_LOAD));
    reset(52000, 3);

    // Loss of communication alone holds the normal output. Loss during correction uses minimum.
    base = 78;
    applied(52100, 78);
    snapshot.state = INA226_OFFLINE;
    snapshot.sample_valid = false;
    expect_step(52200, OUTPUT_LIMIT_NORMAL, 78);
    sample(52300, 55300, threshold + 21);
    expect_step(55300, OUTPUT_LIMIT_VERIFY_FORMULA, 64);
    applied(55400, 64);
    snapshot.state = INA226_OFFLINE;
    snapshot.sample_valid = false;
    expect_step(55500, OUTPUT_LIMIT_MINIMUM, 64);
    expect_step(55600, OUTPUT_LIMIT_MINIMUM, 64);
    assert(errors & DEVICE_ERROR_OUTPUT_SENSOR);
    sample(55700, 58700, threshold - 1);
    expect_step(58700, OUTPUT_LIMIT_NORMAL, 78);

    // A write admitted before the overload completes before the formula uses its settings.
    mode = OUTPUT_LIMIT_MODE_TEMPERATURE;
    base = 78;
    applied(58800, 78);
    assert(output_limit_begin_config_write());
    sample(58900, 61800, threshold * 2);
    expect_step(61800, OUTPUT_LIMIT_WAIT_CONFIG, 78);
    assert(!output_limit_begin_config_write());
    output_limit_end_config_write();
    expect_step(62200, OUTPUT_LIMIT_VERIFY_FORMULA, 39);
    applied(62300, 39);
    sample(62400, 65200, threshold);
    expect_step(65200, OUTPUT_LIMIT_SEARCH_UP, 44);
    applied(65300, 44);
    sample(65400, 68200, threshold + 1);
    expect_step(68200, OUTPUT_LIMIT_VERIFY_STEP, 39);
    applied(68300, 39);
    sample(68400, 71200, threshold);
    expect_step(71200, OUTPUT_LIMIT_LIMITED, 39);
    assert(output_limit_get_status().temporary_minimum == 39);
    reset(72000, 78);

    // Without a new completed window, a probe times out instead of accepting old telemetry.
    mode = OUTPUT_LIMIT_MODE_CONTINUOUS;
    applied(72100, 78);
    sample(72200, 75200, threshold * 2);
    expect_step(75200, OUTPUT_LIMIT_VERIFY_FORMULA, 39);
    applied(75300, 39);
    expect_step(84199, OUTPUT_LIMIT_VERIFY_FORMULA, 39);
    expect_step(84200, OUTPUT_LIMIT_VERIFY_TIMEOUT, 39);
    assert(errors & DEVICE_ERROR_OUTPUT_VERIFY);
    reset(88000, 78);

    mode = OUTPUT_LIMIT_MODE_SERVICE;
    applied(88100, 78);
    sample(88200, 91200, threshold * 2);
    expect_step(91200, OUTPUT_LIMIT_FAULT, 0);
    puts("Current-based output limit, sample timing and transition tests passed");
}
