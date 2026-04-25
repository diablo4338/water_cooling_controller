#ifndef SHARED_I2C_BUS_H
#define SHARED_I2C_BUS_H

#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHARED_I2C_PORT I2C_NUM_0
#define SHARED_I2C_SDA_GPIO 5
#define SHARED_I2C_SCL_GPIO 4

esp_err_t shared_i2c_bus_init(void);
i2c_master_bus_handle_t shared_i2c_bus_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif
