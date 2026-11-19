#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
src="$test_dir/../../src"
work="$(mktemp -d)"
trap 'rm -r -- "$work"' EXIT

mkdir -p "$work/freertos" "$work/host" "$work/driver"
printf '%s\n' '#define BLE_HS_CONN_HANDLE_NONE 0xffff' > "$work/host/ble_hs.h"
printf '%s\n' 'typedef int portMUX_TYPE;' > "$work/freertos/portmacro.h"
cat > "$work/freertos/FreeRTOS.h" <<'EOF'
#pragma once
#include "portmacro.h"
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
EOF
printf '%s\n' '#include <stdint.h>' 'typedef void *esp_timer_handle_t;' 'int64_t esp_timer_get_time(void);' > "$work/esp_timer.h"
printf '%s\n' '#pragma once' '#include <assert.h>' '#define ESP_ERROR_CHECK(result) assert((result) == 0)' > "$work/esp_err.h"
printf '%s\n' '#include <stdio.h>' '#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)' \
    '#define ESP_LOGE ESP_LOGW' \
    > "$work/esp_log.h"

"${CC:-cc}" -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I"$work" -I"$src/ble_protocol/control" -I"$src/ble_protocol/hw" -I"$src/ble_protocol/pairing" -I"$src/metrics" \
    "$src/ble_protocol/control/output_limit.c" "$test_dir/test_output_limit.c" \
    -lm -o "$work/test_output_limit"
"$work/test_output_limit"

cat > "$work/freertos/semphr.h" <<'EOF'
typedef int StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY 0
#define xSemaphoreCreateMutexStatic(storage) (storage)
#define xSemaphoreTake(mutex, delay) ((void)(mutex), (void)(delay))
#define xSemaphoreGive(mutex) ((void)(mutex))
EOF
cat > "$work/nvs.h" <<'EOF'
#include <stddef.h>
#include <stdint.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
EOF

"${CC:-cc}" -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I"$work" -I"$src/ble_protocol/control" -I"$src/ble_protocol/hw" -I"$src/ble_protocol/pairing" -I"$src/metrics" \
    "$src/ble_protocol/control/params.c" "$src/ble_protocol/control/output_limit.c" \
    "$test_dir/test_profile_limit.c" -lm -o "$work/test_profile_limit"
"$work/test_profile_limit"

cat >> "$work/freertos/FreeRTOS.h" <<'EOF'
typedef unsigned TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define pdFALSE 0
EOF
cat > "$work/freertos/task.h" <<'EOF'
TickType_t xTaskGetTickCount(void);
int xTaskDelayUntil(TickType_t *wake, TickType_t period);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle);
EOF
cat >> "$work/esp_err.h" <<'EOF'
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM };
const char *esp_err_to_name(esp_err_t err);
EOF
printf '%s\n' '#define ESP_LOGI ESP_LOGW' >> "$work/esp_log.h"
touch "$work/driver/i2c_types.h"
cat > "$work/driver/i2c_master.h" <<'EOF'
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct {
    int dev_addr_length, device_address, scl_speed_hz, scl_wait_us;
    struct { int disable_ack_check; } flags;
} i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 7
int i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t n, int timeout);
int i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *reg, size_t reg_size,
                                uint8_t *data, size_t n, int timeout);
int i2c_master_bus_reset(i2c_master_bus_handle_t bus);
int i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *device);
EOF

"${CC:-cc}" -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I"$work" -I"$src/i2c_bus" -I"$src/metrics" \
    "$src/metrics/ina226.c" "$test_dir/test_ina226_monitor.c" -o "$work/test_ina226_monitor"
"$work/test_ina226_monitor"
