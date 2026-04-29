#ifndef INA226_H
#define INA226_H

#include <stdbool.h>
#include <stdint.h>

#define INA226_ADDR        0x40

#define REG_CONFIG         0x00
#define REG_SHUNT_VOLTAGE  0x01
#define REG_BUS_VOLTAGE    0x02
#define REG_POWER          0x03
#define REG_CURRENT        0x04
#define REG_CALIBRATION    0x05
#define REG_MASK_ENABLE    0x06
#define REG_ALERT_LIMIT    0x07

#define CAL_VALUE  2179  // 0x0883

#define INA226_I2C_FREQ_HZ 400000

#ifndef INA226_ALERT_GPIO
#define INA226_ALERT_GPIO 8
#endif

typedef struct {
    uint16_t bus_raw;
    int16_t current_raw;
    float voltage_v;
    float current_ma;
    bool overcurrent;
    bool valid;
} ina226_sample_t;

bool ina226_init(void);
bool ina226_has_error(void);
bool ina226_get_sample(ina226_sample_t *out);
bool ina226_overcurrent_active(void);
float ina226_bus_raw_to_v(uint16_t raw);
float ina226_current_raw_to_ma(int16_t raw);

#endif
