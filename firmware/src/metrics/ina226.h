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

#define CAL_VALUE  2179  // 0x0883
#define INA226_OVERCURRENT_THRESHOLD_MA 500U
#define INA226_CURRENT_LSB_UA 50U

#define INA226_I2C_FREQ_HZ 400000

typedef enum {
    INA226_WAITING = 0,
    INA226_READY,
    INA226_OFFLINE,
} ina226_state_t;

typedef struct {
    ina226_state_t state;
    bool sample_valid;
    float voltage_v;
    float current_ma;
    int64_t conversion_started_after_us; // Conservative lower bound for the averaging window start.
    int64_t sampled_us;
    uint32_t sequence;
} ina226_snapshot_t;

void ina226_monitor_start(void);
ina226_snapshot_t ina226_get_snapshot(void);

#endif
