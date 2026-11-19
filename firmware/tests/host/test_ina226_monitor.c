#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

#include "ina226.h"
#include "shared_i2c_bus.h"
#include "freertos/task.h"

#define CONVERSION_READY (1U << 3)

static uint16_t registers[8];
static unsigned polls;
static unsigned data_reads;
static bool fail_poll;
static bool convert_during_read;
static void (*run_monitor)(void *);
static jmp_buf stop_monitor;

const char *esp_err_to_name(esp_err_t err) { return "io error"; }
i2c_master_bus_handle_t shared_i2c_bus_get_handle(void) { return registers; }
bool shared_i2c_bus_lock(TickType_t timeout) { return true; }
void shared_i2c_bus_unlock(void) {}
int i2c_master_bus_reset(i2c_master_bus_handle_t bus) { return ESP_OK; }
int i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *device) {
    *device = bus;
    return ESP_OK;
}

int i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t n, int timeout) {
    assert(n == 3 && data[0] < 8);
    registers[data[0]] = (uint16_t)(data[1] << 8 | data[2]);
    return ESP_OK;
}

int i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *reg, size_t reg_size,
                                uint8_t *data, size_t n, int timeout) {
    assert(reg_size == 1 && n == 2 && *reg < 8);
    if (fail_poll) return ESP_ERR_TIMEOUT;
    uint16_t value = registers[*reg];
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
    if (*reg == REG_MASK_ENABLE) registers[*reg] &= ~CONVERSION_READY;
    if (*reg == REG_CURRENT) {
        data_reads++;
        if (convert_during_read) registers[REG_MASK_ENABLE] |= CONVERSION_READY;
    }
    return ESP_OK;
}

int64_t esp_timer_get_time(void) { return (polls + 1) * 500000LL; }
TickType_t xTaskGetTickCount(void) { return polls * 500; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    assert(!run_monitor);
    run_monitor = task;
    return pdPASS;
}

int xTaskDelayUntil(TickType_t *wake, TickType_t period) {
    assert(period == 500 && *wake == polls * period);
    *wake += period;
    ina226_snapshot_t sample = ina226_get_snapshot();
    switch (polls++) {
        case 0:
            assert(sample.state == INA226_WAITING && !sample.sample_valid && data_reads == 0);
            assert(registers[REG_MASK_ENABLE] == 0);
            registers[REG_MASK_ENABLE] |= CONVERSION_READY;
            registers[REG_CURRENT] = 2000;
            break;
        case 1:
            assert(sample.state == INA226_READY && sample.sample_valid && sample.sequence == 1);
            assert(sample.current_ma == 100.0f && sample.sampled_us == 1000000 && data_reads == 1);
            assert(sample.conversion_started_after_us == 0);
            break;
        case 2:
            assert(sample.state == INA226_WAITING && sample.sample_valid && sample.sequence == 1);
            assert(sample.current_ma == 100.0f && sample.sampled_us == 1000000 && data_reads == 1);
            registers[REG_MASK_ENABLE] |= CONVERSION_READY;
            registers[REG_CURRENT] = 2200;
            convert_during_read = true;
            break;
        case 3:
            assert(sample.state == INA226_WAITING && sample.sample_valid && sample.sequence == 1);
            assert(sample.current_ma == 100.0f && sample.sampled_us == 1000000 && data_reads == 2);
            assert(sample.conversion_started_after_us == 0);
            convert_during_read = false;
            registers[REG_MASK_ENABLE] |= CONVERSION_READY;
            break;
        case 4:
            assert(sample.state == INA226_READY && sample.sample_valid && sample.sequence == 3);
            assert(sample.sampled_us == 2500000 && sample.conversion_started_after_us == 2000000);
            fail_poll = true;
            break;
        case 5:
            assert(sample.state == INA226_OFFLINE && !sample.sample_valid && sample.sampled_us == 2500000);
            fail_poll = false;
            break;
        case 6:
            assert(sample.state == INA226_WAITING && !sample.sample_valid && sample.sequence == 3);
            registers[REG_MASK_ENABLE] |= CONVERSION_READY;
            registers[REG_CURRENT] = 2100;
            break;
        default:
            assert(sample.state == INA226_READY && sample.sample_valid && sample.sequence == 4);
            assert(sample.current_ma == 105.0f && sample.sampled_us == 4000000 && data_reads == 4);
            assert(sample.conversion_started_after_us == 0);
            longjmp(stop_monitor, 1);
    }
    return pdPASS;
}

int main(void) {
    ina226_monitor_start();
    ina226_monitor_start();
    if (setjmp(stop_monitor) == 0) run_monitor(NULL);
    puts("INA226 monitor cadence, cache, and recovery tests passed");
}
