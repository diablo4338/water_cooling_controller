#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
src="$test_dir/../../src"
work="$(mktemp -d)"
trap 'rm -r -- "$work"' EXIT

# The operation manager uses these ESP-IDF headers for the clock and BLE owner ID.
mkdir -p "$work/freertos" "$work/host"
touch "$work/esp_log.h" "$work/freertos/FreeRTOS.h"
printf '%s\n' 'typedef int portMUX_TYPE;' > "$work/freertos/portmacro.h"
printf '%s\n' '#include <stdint.h>' 'int64_t esp_timer_get_time(void);' \
    'typedef void *esp_timer_handle_t;' > "$work/esp_timer.h"
printf '%s\n' '#define BLE_HS_CONN_HANDLE_NONE 0xffff' > "$work/host/ble_hs.h"

"${CC:-cc}" -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I"$work" -I"$src/ble_protocol/ble" -I"$src/ble_protocol/control" \
    -I"$src/ble_protocol/pairing" -I"$src/metrics" \
    "$src/ble_protocol/control/operation_manager.c" "$test_dir/test_fan_calibration.c" \
    -lm -o "$work/test_fan_calibration"
"$work/test_fan_calibration"
