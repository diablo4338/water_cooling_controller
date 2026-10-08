#include "unity.h"
#include "runtime_config.h"
#include "nvs.h"

void runtime_config_tests_force_link(void) {}

TEST_CASE("global reset persists factory runtime config", "[config]") {
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_result_t result;
    runtime_config_encode(wire, 1);
    wire[3] = 50;  /* idle current: 50 mA */
    wire[4] = 0;
    wire[13] = 35; /* button delta: 3.5 C */
    wire[14] = 0;
    wire[17] = 0x40; /* buzzer threshold: 8 V */
    wire[18] = 0x1f;
    wire[21] = 0xe8; /* button signal: 1 s */
    wire[22] = 0x03;
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    TEST_ASSERT_EQUAL(CONFIG_STATUS_OK, result.status);

    runtime_config_t config;
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(50, config.idle_current_ma);
    TEST_ASSERT_EQUAL(35, config.button_delta_deci_c);
    TEST_ASSERT_EQUAL(8000, config.buzzer_threshold_mv);
    TEST_ASSERT_EQUAL(1000, config.buzzer_button_ms);
    TEST_ASSERT_EQUAL(1, config.buzzer_enabled);

    runtime_config_encode(wire, 2);
    wire[27] = 0;
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    TEST_ASSERT_EQUAL(CONFIG_STATUS_OK, result.status);
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(0, config.buzzer_enabled);

    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(100, config.idle_current_ma);
    TEST_ASSERT_EQUAL(6600, config.voltage_min_mv);
    TEST_ASSERT_EQUAL(8400, config.voltage_max_mv);
    TEST_ASSERT_EQUAL(20, config.button_delta_deci_c);
    TEST_ASSERT_EQUAL(300, config.idle_timeout_s);
    TEST_ASSERT_EQUAL(5000, config.deep_sleep_ms);
    TEST_ASSERT_EQUAL(7000, config.buzzer_threshold_mv);
    TEST_ASSERT_EQUAL(10, config.buzzer_low_delay_s);
    TEST_ASSERT_EQUAL(500, config.buzzer_button_ms);
    TEST_ASSERT_EQUAL(1000, config.buzzer_low_ms);
    TEST_ASSERT_EQUAL(1500, config.buzzer_combined_ms);
    TEST_ASSERT_EQUAL(1, config.buzzer_enabled);

    runtime_config_init(); /* Reload the committed blob as on the next boot. */
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(100, config.idle_current_ma);
    TEST_ASSERT_EQUAL(6600, config.voltage_min_mv);
    TEST_ASSERT_EQUAL(8400, config.voltage_max_mv);
    TEST_ASSERT_EQUAL(20, config.button_delta_deci_c);
    TEST_ASSERT_EQUAL(300, config.idle_timeout_s);
    TEST_ASSERT_EQUAL(5000, config.deep_sleep_ms);
    TEST_ASSERT_EQUAL(7000, config.buzzer_threshold_mv);
    TEST_ASSERT_EQUAL(10, config.buzzer_low_delay_s);
    TEST_ASSERT_EQUAL(500, config.buzzer_button_ms);
    TEST_ASSERT_EQUAL(1000, config.buzzer_low_ms);
    TEST_ASSERT_EQUAL(1500, config.buzzer_combined_ms);
    TEST_ASSERT_EQUAL(1, config.buzzer_enabled);
}

TEST_CASE("version two settings keep their values and gain buzzer defaults", "[config]") {
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 0);
    wire[0] = 2;
    wire[3] = 75; /* idle current */
    wire[13] = 35; /* temperature delta */

    nvs_handle_t handle;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("runtime_cfg", NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(handle, "values", wire, 17));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    runtime_config_init();
    runtime_config_t config;
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(75, config.idle_current_ma);
    TEST_ASSERT_EQUAL(35, config.button_delta_deci_c);
    TEST_ASSERT_EQUAL(7000, config.buzzer_threshold_mv);
    TEST_ASSERT_EQUAL(10, config.buzzer_low_delay_s);
    TEST_ASSERT_EQUAL(500, config.buzzer_button_ms);
    TEST_ASSERT_EQUAL(1000, config.buzzer_low_ms);
    TEST_ASSERT_EQUAL(1500, config.buzzer_combined_ms);
    TEST_ASSERT_EQUAL(1, config.buzzer_enabled);
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
}

TEST_CASE("version three buzzer settings survive migration", "[config]") {
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 0);
    wire[0] = 3;
    wire[17] = 0xf8; /* 11 V from the previous firmware */
    wire[18] = 0x2a;
    wire[21] = 0xe8; /* 1 second button signal */
    wire[22] = 0x03;

    nvs_handle_t handle;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("runtime_cfg", NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(handle, "values", wire, 27));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    runtime_config_init();
    runtime_config_t config;
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(11000, config.buzzer_threshold_mv);
    TEST_ASSERT_EQUAL(1000, config.buzzer_button_ms);
    TEST_ASSERT_EQUAL(1, config.buzzer_enabled);
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
}

TEST_CASE("runtime config accepts numeric values representable on the wire", "[config]") {
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 3);
    wire[5] = 0x40; /* lower: 40,000 mV */
    wire[6] = 0x9c;
    wire[7] = 0x50; /* upper: 50,000 mV */
    wire[8] = 0xc3;
    wire[3] = 0xff; /* idle current: maximum uint16 */
    wire[4] = 0xff;
    wire[9] = 0; /* timeout: zero */
    wire[10] = 0;
    wire[11] = 0;
    wire[12] = 0;
    wire[13] = 0; /* thermal delta: zero */
    wire[14] = 0;
    wire[21] = 0; /* button signal: zero */
    wire[22] = 0;
    runtime_config_result_t result;
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    TEST_ASSERT_EQUAL(CONFIG_STATUS_OK, result.status);
    runtime_config_t config;
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(40000, config.voltage_min_mv);
    TEST_ASSERT_EQUAL(50000, config.voltage_max_mv);
    TEST_ASSERT_EQUAL(65535, config.idle_current_ma);
    TEST_ASSERT_EQUAL(0, config.idle_timeout_s);
    TEST_ASSERT_EQUAL(0, config.button_delta_deci_c);
    TEST_ASSERT_EQUAL(0, config.buzzer_button_ms);
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
}

TEST_CASE("voltage upper bound must be strictly above lower bound", "[config]") {
    TEST_ASSERT_EQUAL(ESP_OK, runtime_config_reset_defaults());
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 4);
    wire[7] = wire[5];
    wire[8] = wire[6];
    runtime_config_result_t result;
    runtime_config_apply_wire(wire, sizeof(wire), &result);
    TEST_ASSERT_EQUAL(CONFIG_STATUS_VOLTAGE_ORDER, result.status);
    TEST_ASSERT_EQUAL(CONFIG_FIELD_VOLTAGE_MAX, result.field);
    runtime_config_t config;
    runtime_config_get(&config);
    TEST_ASSERT_EQUAL(6600, config.voltage_min_mv);
    TEST_ASSERT_EQUAL(8400, config.voltage_max_mv);
}
