#ifndef DRIVER_I2C_MASTER_H
#define DRIVER_I2C_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;

#define I2C_ADDR_BIT_LEN_7 0
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0

typedef struct {
    uint8_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    uint32_t scl_wait_us;
    struct {
        uint32_t disable_ack_check;
    } flags;
} i2c_device_config_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data, size_t size,
                              int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *write_data,
                                      size_t write_size, uint8_t *read_data,
                                      size_t read_size, int timeout_ms);
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus);

#endif
