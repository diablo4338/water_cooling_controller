#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "runtime_config.h"

static uint8_t stored_blob[64];
static size_t stored_length;
static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

esp_err_t nvs_open(const char *namespace_name, int mode, nvs_handle_t *handle) {
    (void)namespace_name;
    if (!handle || (mode == NVS_READONLY && stored_length == 0)) return ESP_FAIL;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length) {
    (void)key;
    if (handle != 1 || !length || stored_length == 0) return ESP_FAIL;
    if (*length < stored_length) {
        *length = stored_length;
        return ESP_FAIL;
    }
    memcpy(out_value, stored_blob, stored_length);
    *length = stored_length;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length) {
    (void)key;
    if (handle != 1 || !value || length > sizeof(stored_blob)) return ESP_FAIL;
    memcpy(stored_blob, value, length);
    stored_length = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    return handle == 1 ? ESP_OK : ESP_FAIL;
}

void nvs_close(nvs_handle_t handle) {
    (void)handle;
}

static uint16_t read_u16(const uint8_t *wire, size_t offset) {
    return (uint16_t)wire[offset] | ((uint16_t)wire[offset + 1U] << 8);
}

static void write_u16(uint8_t *wire, size_t offset, uint16_t value) {
    wire[offset] = (uint8_t)value;
    wire[offset + 1U] = (uint8_t)(value >> 8);
}

static void test_v6_round_trip_and_ranges(void) {
    CHECK(runtime_config_reset_defaults() == ESP_OK);
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 0x1234);
    CHECK(sizeof(wire) == 34U);
    CHECK(wire[0] == 6U);
    CHECK(wire[1] == 0x34U && wire[2] == 0x12U);
    CHECK(read_u16(wire, 28) == 1000U);
    CHECK(read_u16(wire, 30) == 100U);
    CHECK(read_u16(wire, 32) == 2220U);

    runtime_config_result_t result;
    write_u16(wire, 28, 1000U);
    write_u16(wire, 30, 1200U);
    write_u16(wire, 32, 3000U);
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    CHECK(result.status == CONFIG_STATUS_OK);
    runtime_config_t config;
    runtime_config_get(&config);
    CHECK(config.charging_blink_interval_ms == 1000U);
    CHECK(config.wheel_min_speed_deci_kmh == 1200U);
    CHECK(config.wheel_circumference_mm == 3000U);

    write_u16(wire, 28, 999U);
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    CHECK(result.status == CONFIG_STATUS_RANGE);
    CHECK(result.field == CONFIG_FIELD_CHARGING_BLINK_INTERVAL);
    CHECK(runtime_config_reset_defaults() == ESP_OK);
}

static void test_only_current_format_is_accepted(void) {
    const size_t lengths[] = {19, 17, 27, 28, 34};
    for (unsigned version = 1; version < RUNTIME_CONFIG_VERSION; ++version) {
        CHECK(runtime_config_reset_defaults() == ESP_OK);
        uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
        runtime_config_encode(wire, 42);
        wire[0] = version;
        write_u16(wire, 3, 77);
        runtime_config_result_t result;
        runtime_config_apply_wire(wire, lengths[version - 1], &result);
        CHECK(result.status == CONFIG_STATUS_FORMAT);
        CHECK(result.request_id == 42);
        runtime_config_t config;
        runtime_config_get(&config);
        CHECK(config.idle_current_ma == 100);
        CHECK(stored_blob[0] == RUNTIME_CONFIG_VERSION);
        memcpy(stored_blob, wire, lengths[version - 1]);
        stored_length = lengths[version - 1];
        runtime_config_init();
        runtime_config_get(&config);
        CHECK(config.idle_current_ma == 100);
        CHECK(config.wheel_circumference_mm == 2220);
    }
    CHECK(runtime_config_reset_defaults() == ESP_OK);
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 1);
    write_u16(wire, 3, 77);
    runtime_config_result_t result;
    runtime_config_apply_wire(wire, sizeof(wire) - 1, &result);
    CHECK(result.status == CONFIG_STATUS_FORMAT);
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    CHECK(result.status == CONFIG_STATUS_OK);
    runtime_config_init();
    runtime_config_t config;
    runtime_config_get(&config);
    CHECK(config.idle_current_ma == 77);
    CHECK(runtime_config_reset_defaults() == ESP_OK);
}

static void test_circumference_boundaries(void) {
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 0);
    runtime_config_result_t result;
    const uint16_t values[] = {899, 900, 10000, 10001};
    for (size_t i = 0; i < 4; ++i) {
        write_u16(wire, 32, values[i]);
        runtime_config_apply_wire(wire, sizeof(wire), &result);
        CHECK(result.status == ((i == 0 || i == 3) ? CONFIG_STATUS_RANGE : CONFIG_STATUS_OK));
    }
    CHECK(runtime_config_reset_defaults() == ESP_OK);
}

int main(void) {
    test_v6_round_trip_and_ranges();
    test_only_current_format_is_accepted();
    test_circumference_boundaries();
    if (failures != 0) {
        fprintf(stderr, "%d runtime config assertions failed\n", failures);
        return 1;
    }
    puts("runtime config host tests passed");
    return 0;
}
