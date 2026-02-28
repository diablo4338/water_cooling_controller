#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"

#include "ads1115.h"

void metrics_tests_force_link(void) {}

TEST_CASE("i2c bus alive (ads1115 responds)", "[metrics]") {
    ads1115_init();

    uint8_t reg = 0x01;
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_write_read_device(
        ADS1115_I2C_PORT, ADS1115_I2C_ADDR, &reg, 1, data, sizeof(data), pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

TEST_CASE("ads1115 read from channel A0", "[metrics]") {
    ads1115_init();

    int16_t raw = 0;
    bool ok = ads1115_read_raw(0, &raw);
    TEST_ASSERT_TRUE(ok);
}
