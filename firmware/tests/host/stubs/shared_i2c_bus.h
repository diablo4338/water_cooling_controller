#ifndef SHARED_I2C_BUS_H
#define SHARED_I2C_BUS_H

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"

#define SHARED_I2C_PORT I2C_NUM_0
#define SHARED_I2C_SDA_GPIO 5
#define SHARED_I2C_SCL_GPIO 4

i2c_master_bus_handle_t shared_i2c_bus_get_handle(void);
bool shared_i2c_bus_lock(TickType_t timeout_ticks);
void shared_i2c_bus_unlock(void);

#endif
