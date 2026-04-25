#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#include "ads1115.h"
#include "shared_i2c_bus.h"

void metrics_tests_force_link(void) {}

TEST_CASE("i2c bus alive (ads1115 responds)", "[metrics]") {
    TEST_ASSERT_EQUAL(ESP_OK, shared_i2c_bus_init());
    ads1115_init();

    i2c_master_bus_handle_t bus = NULL;
    bus = shared_i2c_bus_get_handle();
    TEST_ASSERT_NOT_NULL(bus);

    esp_err_t err = i2c_master_probe(bus, ADS1115_I2C_ADDR, 100);
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

TEST_CASE("ads1115 read from channel A0", "[metrics]") {
    TEST_ASSERT_EQUAL(ESP_OK, shared_i2c_bus_init());
    ads1115_init();

    int16_t raw = 0;
    bool ok = ads1115_read_raw(0, &raw);
    TEST_ASSERT_TRUE(ok);
}
