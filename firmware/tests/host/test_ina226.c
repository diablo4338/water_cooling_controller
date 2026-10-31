#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ina226.h"
#include "shared_i2c_bus.h"

static uint64_t s_elapsed_ms;
static unsigned s_mask_reads;
static unsigned s_ready_poll;
static unsigned s_fail_mask_read;
static uint8_t s_fail_register;
static unsigned s_fail_register_count;
static unsigned s_reset_count;
static unsigned s_lock_count;
static unsigned s_transfer_count;

void vTaskDelay(TickType_t ticks) {
    s_elapsed_ms += ticks;
}

i2c_master_bus_handle_t shared_i2c_bus_get_handle(void) {
    return (i2c_master_bus_handle_t)1;
}

bool shared_i2c_bus_lock(TickType_t timeout_ticks) {
    ++s_lock_count;
    s_elapsed_ms += timeout_ticks;
    return true;
}

void shared_i2c_bus_unlock(void) {}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device) {
    (void)bus;
    (void)config;
    *device = (i2c_master_dev_handle_t)1;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data, size_t size,
                              int timeout_ms) {
    (void)device;
    (void)data;
    (void)size;
    ++s_transfer_count;
    s_elapsed_ms += (uint64_t)timeout_ms;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *write_data,
                                      size_t write_size, uint8_t *read_data,
                                      size_t read_size, int timeout_ms) {
    (void)device;
    (void)write_size;
    ++s_transfer_count;
    s_elapsed_ms += (uint64_t)timeout_ms;
    uint8_t reg = write_data[0];
    if (reg == REG_MASK_ENABLE) {
        ++s_mask_reads;
        if (s_mask_reads == s_fail_mask_read) return ESP_ERR_TIMEOUT;
        if (s_ready_poll != 0U &&
            s_mask_reads == s_ready_poll + 1U) {
            read_data[0] = 0;
            read_data[1] = 1U << 3;
            return ESP_OK;
        }
    }
    if (reg == s_fail_register && s_fail_register_count > 0U) {
        --s_fail_register_count;
        return ESP_ERR_TIMEOUT;
    }
    memset(read_data, 0, read_size);
    return ESP_OK;
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus) {
    (void)bus;
    ++s_reset_count;
    s_elapsed_ms += 50U;
    return ESP_OK;
}

int main(void) {
    ina226_sample_t sample;
    esp_err_t error = ESP_FAIL;

    s_ready_poll = 40U;
    s_fail_register = REG_CURRENT;
    s_fail_register_count = 1U;
    if (ina226_get_single_sample_ex(&sample, &error) ||
        error != ESP_ERR_TIMEOUT || s_elapsed_ms != 10450U ||
        s_reset_count != 1U) {
        fprintf(stderr, "first-use worst path was not bounded: err=%d time=%llu\n",
                error, (unsigned long long)s_elapsed_ms);
        return EXIT_FAILURE;
    }

    s_elapsed_ms = 0;
    s_mask_reads = 0;
    s_ready_poll = 40U;
    s_fail_register = 0;
    s_fail_register_count = 0;
    if (!ina226_get_single_sample_ex(&sample, &error) ||
        s_elapsed_ms != 10200U) {
        fprintf(stderr, "late-ready sample was not bounded: err=%d time=%llu\n",
                error, (unsigned long long)s_elapsed_ms);
        return EXIT_FAILURE;
    }

    s_elapsed_ms = 0;
    s_ready_poll = 0;
    s_mask_reads = 1U;
    if (ina226_get_single_sample_ex(&sample, &error) ||
        error != ESP_ERR_TIMEOUT || s_elapsed_ms > 8800U ||
        s_reset_count != 2U) {
        fprintf(stderr, "conversion timeout was not bounded: err=%d time=%llu\n",
                error, (unsigned long long)s_elapsed_ms);
        return EXIT_FAILURE;
    }

    s_elapsed_ms = 0;
    s_mask_reads = 0U;
    s_fail_mask_read = 2U;
    if (ina226_get_single_sample_ex(&sample, &error) ||
        error != ESP_ERR_TIMEOUT || s_elapsed_ms > 2000U ||
        s_reset_count != 3U) {
        fprintf(stderr, "I2C timeout was not returned: err=%d time=%llu\n",
                error, (unsigned long long)s_elapsed_ms);
        return EXIT_FAILURE;
    }
    puts("INA226 bounded-path tests passed (10.45 s worst expected execution)");
    return EXIT_SUCCESS;
}
