#ifndef ADS1115_H
#define ADS1115_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c.h"

#define ADS1115_I2C_PORT      I2C_NUM_0
#define ADS1115_I2C_ADDR      0x48
#define ADS1115_I2C_FREQ_HZ   400000
#define ADS1115_I2C_SDA_GPIO  8
#define ADS1115_I2C_SCL_GPIO  9

#define ADS1115_CONV_DELAY_MS     10
#define ADS1115_INTER_CH_DELAY_MS 5
#define ADS1115_FS_V              4.096f

void ads1115_init(void);
bool ads1115_read_raw(uint8_t channel, int16_t *out_raw);
float ads1115_raw_to_v(int16_t raw);
float ads1115_raw_to_temp(int16_t raw);

#endif
