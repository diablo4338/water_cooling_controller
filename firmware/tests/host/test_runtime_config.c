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

static void test_v4_migration_and_write_compatibility(void) {
    CHECK(runtime_config_reset_defaults() == ESP_OK);
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 0);
    wire[0] = 4;
    write_u16(wire, 3, 75U);
    wire[27] = 0;
    nvs_handle_t handle;
    CHECK(nvs_open("runtime_cfg", NVS_READWRITE, &handle) == ESP_OK);
    CHECK(nvs_set_blob(handle, "values", wire, 28U) == ESP_OK);
    CHECK(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);

    runtime_config_init();
    runtime_config_t config;
    runtime_config_get(&config);
    CHECK(config.idle_current_ma == 75U);
    CHECK(config.buzzer_enabled == 0U);
    CHECK(config.charging_blink_interval_ms == 1000U);
    CHECK(config.wheel_min_speed_deci_kmh == 100U);
    CHECK(config.wheel_circumference_mm == 2220U);

    runtime_config_result_t result;
    runtime_config_encode(wire, 9);
    write_u16(wire, 28, 4000U);
    write_u16(wire, 30, 100U);
    write_u16(wire, 32, 1800U);
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    CHECK(result.status == CONFIG_STATUS_OK);

    runtime_config_encode(wire, 10);
    wire[0] = 4;
    write_u16(wire, 3, 80U);
    runtime_config_apply_wire(wire, 28U, &result);
    CHECK(result.status == CONFIG_STATUS_OK);
    runtime_config_get(&config);
    CHECK(config.idle_current_ma == 80U);
    CHECK(config.charging_blink_interval_ms == 4000U);
    CHECK(config.wheel_min_speed_deci_kmh == 100U);
    CHECK(config.wheel_circumference_mm == 1800U);
    CHECK(stored_length == RUNTIME_CONFIG_WIRE_SIZE);
    CHECK(stored_blob[0] == RUNTIME_CONFIG_VERSION);
    CHECK(runtime_config_reset_defaults() == ESP_OK);
}

static void test_v5_diameter_migration(void) {
    const uint16_t diameters[] = {300, 707, 2100, 3000};
    const uint16_t circumferences[] = {942, 2221, 6597, 9425};
    for (size_t i = 0; i < sizeof(diameters) / sizeof(diameters[0]); ++i) {
        CHECK(runtime_config_reset_defaults() == ESP_OK);
        uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
        runtime_config_encode(wire, 42);
        wire[0] = 5;
        write_u16(wire, 3, 77);
        write_u16(wire, 32, diameters[i]);
        memcpy(stored_blob, wire, sizeof(wire));
        stored_length = sizeof(wire);
        runtime_config_init();
        runtime_config_t config;
        runtime_config_get(&config);
        CHECK(config.idle_current_ma == 77);
        CHECK(config.wheel_circumference_mm == circumferences[i]);
        runtime_config_result_t result;
        runtime_config_apply_wire(wire, sizeof(wire), &result);
        CHECK(result.status == CONFIG_STATUS_OK);
        CHECK(result.request_id == 42);
        CHECK(stored_blob[0] == 6);
        CHECK(read_u16(stored_blob, 32) == circumferences[i]);
        runtime_config_init();
        runtime_config_get(&config);
        CHECK(config.wheel_circumference_mm == circumferences[i]);
    }
    CHECK(runtime_config_reset_defaults() == ESP_OK);
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
    test_v5_diameter_migration();
    test_v4_migration_and_write_compatibility();
    if (failures != 0) {
        fprintf(stderr, "%d runtime config assertions failed\n", failures);
        return 1;
    }
    puts("runtime config host tests passed");
    return 0;
}
