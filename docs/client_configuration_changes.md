## Sleep type, protocol v7

Runtime configuration now uses version 7 and a 35-byte payload. Bytes 0–33 retain
the v6 layout. Byte 34 is `sleep_type`, an unsigned byte with values
`0` for deep sleep and `1` for light sleep. The default is deep sleep.
Use a “Тип сна” selector with “Тяжёлый” and “Лёгкий” choices.
Invalid values return `CONFIG_STATUS_RANGE` with field ID 17.
The existing `deep_sleep_ms` interval applies to both modes.
Result messages also carry version 7. Clients must send the complete v7 payload.
Stored v6 NVS settings are loaded with deep sleep selected.

Light sleep stops NimBLE using `nimble_port_stop()`, waits for the host loop
to return, deletes its FreeRTOS task, then calls `nimble_port_deinit()`.
ESP-IDF 5.4.4 implements controller disable/deinit inside that last call.
BLE starts through the existing button event handling. Waking from light sleep
does not initialize BLE. The scheduler resumes with its existing deadlines.

References

- [ESP-IDF 5.4 sleep modes](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c3/api-reference/system/sleep_modes.html)
- [ESP-IDF 5.4 NimBLE API](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c3/api-reference/bluetooth/nimble/index.html)

The remaining sections describe the preceding v6 layout.

# BLE client configuration changes

This document describes the implemented firmware protocol in configuration
version 6. The main BLE service UUID is
`0814951d-ca1b-4581-b1af-9c4f2ca96e90`.

## Characteristics

| Purpose | UUID | Properties | Payload |
|---|---|---|---|
| Runtime configuration | `dc881986-aada-4764-bfb9-d79326a6c03a` | Read, Write | Versioned configuration blob, 34 bytes in v6 |
| Configuration result | `11d1283c-ad34-406d-9d96-7a11c69db086` | Read, Notify | 5-byte result |
| Load state metric | `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718281` | Read, Notify | One-byte state value |

Configuration access requires the existing authenticated data-access
permission. Write the entire configuration blob; individual fields are not
written separately. A successful write is persisted to NVS before the active
configuration changes. The result characteristic also sends a notification
after each write.

## Configuration version and wire layout

Configuration v6 is 34 bytes and is the only supported format. All multibyte
integers are unsigned and little-endian. Offsets are zero-based.

| Offset | Size | Field / header | Type | v6 behavior |
|---:|---:|---|---|---|
| 0 | 1 | `version` | `uint8` | Must be `6` for v6 writes |
| 1 | 2 | `request_id` | `uint16` | Client correlation ID; echoed in the result |
| 3 | 2 | `idle_current_ma` | `uint16` | Existing field; idle and charging current threshold |
| 5 | 2 | `voltage_min_mv` | `uint16` | Existing field; battery display lower voltage bound |
| 7 | 2 | `voltage_max_mv` | `uint16` | Existing field |
| 9 | 4 | `idle_timeout_s` | `uint32` | Existing field |
| 13 | 2 | `button_delta_deci_c` | `uint16` | Existing field |
| 15 | 2 | `deep_sleep_ms` | `uint16` | Existing field |
| 17 | 2 | `buzzer_threshold_mv` | `uint16` | Existing field |
| 19 | 2 | `buzzer_low_delay_s` | `uint16` | Existing field |
| 21 | 2 | `buzzer_button_ms` | `uint16` | Existing field |
| 23 | 2 | `buzzer_low_ms` | `uint16` | Existing field |
| 25 | 2 | `buzzer_combined_ms` | `uint16` | Existing field |
| 27 | 1 | `buzzer_enabled` | `uint8` | Existing field; `0` or `1` |
| 28 | 2 | `charging_blink_interval_ms` | `uint16` | Duration of each ON and OFF phase |
| 30 | 2 | `wheel_min_speed_deci_kmh` | `uint16` | Speed stored in 0.1 km/h units |
| 32 | 2 | `wheel_circumference_mm` | `uint16` | Circumference in mm |

### Configuration parameters

Every listed configuration field is stored together in the `runtime_cfg/values`
NVS blob and is accessible through the runtime-configuration characteristic.
Every field is writable as part of the full configuration blob and readable
from the same characteristic. The firmware does not require a restart for the
charging blink interval or thresholds; wheel speed and circumference changes are
used by the reed detector on the next startup/wakeup cycle.

Thus, for each field in the table: NVS = yes; BLE = yes; access = readable and
writable; restart = no, except `wheel_min_speed_deci_kmh` and
`wheel_circumference_mm`, which are picked up by the detector on the next
startup/wakeup cycle (not immediately in the current cycle).

For fields whose validation is type-only, the accepted minimum and
maximum below are the full unsigned representation range. `voltage_max_mv` must
also be greater than `voltage_min_mv`.

| Exact field name | Description and units | Type / size | Default | Accepted values | Suggested UI |
|---|---|---:|---:|---|---|
| `idle_current_ma` | Low-current threshold, mA; also used by NEED_CHARGING confirmation and blink measurement | `uint16`, 2 bytes | 100 | 0–65535 | “Low current threshold”, numeric mA input |
| `voltage_min_mv` | Battery display lower voltage bound, mV | `uint16`, 2 bytes | 6600 | 0–65535; must be less than `voltage_max_mv` | “Low battery threshold”, numeric mV input |
| `voltage_max_mv` | Existing upper voltage bound, mV | `uint16`, 2 bytes | 8400 | 0–65535; must be greater than `voltage_min_mv` | “High battery threshold”, numeric mV input |
| `idle_timeout_s` | Existing low-current idle shutdown delay, seconds | `uint32`, 4 bytes | 300 | 0–4294967295 | “Idle timeout”, numeric seconds input |
| `button_delta_deci_c` | Existing thermal-button threshold delta, 0.1 °C | `uint16`, 2 bytes | 20 | 0–65535 | “Button temperature delta”, numeric 0.1 °C input |
| `deep_sleep_ms` | Existing timer wake interval, ms | `uint16`, 2 bytes | 5000 | 0–65535 | “Sleep interval”, numeric ms input |
| `buzzer_threshold_mv` | Existing buzzer battery threshold, mV | `uint16`, 2 bytes | 7000 | 0–65535 | “Buzzer voltage threshold”, numeric mV input |
| `buzzer_low_delay_s` | Existing low-voltage buzzer delay, seconds | `uint16`, 2 bytes | 10 | 0–65535 | “Low-voltage buzzer delay”, numeric seconds input |
| `buzzer_button_ms` | Existing button buzzer duration, ms | `uint16`, 2 bytes | 500 | 0–65535 | “Button beep duration”, numeric ms input |
| `buzzer_low_ms` | Existing low-voltage buzzer duration, ms | `uint16`, 2 bytes | 1000 | 0–65535 | “Low-voltage beep duration”, numeric ms input |
| `buzzer_combined_ms` | Existing combined buzzer duration, ms | `uint16`, 2 bytes | 1500 | 0–65535 | “Combined beep duration”, numeric ms input |
| `buzzer_enabled` | Existing buzzer enable flag | `uint8`, 1 byte | 1 | 0 or 1 | “Enable buzzer”, boolean switch |
| `charging_blink_interval_ms` | Duration of each NEED_CHARGING ON phase and each OFF phase, ms | `uint16`, 2 bytes | 1000 | 1000–60000 | “Charging blink interval”, numeric ms input |
| `wheel_min_speed_deci_kmh` | Minimum speed to confirm motion, in 0.1 km/h (100 means 10.0 km/h) | `uint16`, 2 bytes | 100 | 10–1200 (1.0–120.0 km/h) | “Minimum wheel speed”, decimal km/h input converted to tenths |
| `wheel_circumference_mm` | Wheel circumference used for one-revolution timing, mm | `uint16`, 2 bytes | 2220 | 900-10000 | Wheel circumference, numeric mm input |

### Global buzzer switch

`buzzer_enabled = 0` suppresses every type of new sound, including button,
startup, low-voltage, shutdown and measurement-error cues. The playback
function reads the current parameter immediately before executing each beep
and completes a muted command silently. An already playing sound may finish.
Changing the flag does not flush queues, interrupt playback or change sleep
policy. Deep sleep resets runtime queues; active BLE can keep the controller
awake, in which case the same local check applies. Re-enabling permits normal
execution of any remaining commands; there is no special stale-command filter.
See the [buzzer contract](../firmware/docs/architecture/04-buzzer-fsm.md).

The NEED_CHARGING entry thresholds are the existing `buzzer_threshold_mv`
and `idle_current_ma` settings; there are no duplicate threshold fields.
`voltage_min_mv` is the battery display lower bound, not the charging-state
threshold. A zero `buzzer_threshold_mv` disables low-voltage entry. Low voltage and low current must both be confirmed for five
seconds of RTC-backed wall time, including Deep Sleep between valid samples.
The pending confirmation and its start timestamp survive timer wakeups. A failed
reading or a valid sample outside either low threshold resets confirmation;
time without measurements is not independently treated as a low reading.
The current condition uses absolute measured current and is strictly
below the threshold. Invalid or missing measurements do not count as low.
NEED_CHARGING returns to WORKING when a fresh correlated ON-phase measurement
has voltage at/above the current `buzzer_threshold_mv` or absolute current above
`idle_current_ma`. Lowering the threshold through BLE takes effect on that next
measurement; setting it to zero also permits recovery. Periodic readings during
blink phases and stale results cannot trigger recovery.

NEED_CHARGING is silent, including thermal presses and ON/OFF blink cycles;
entry cancels any active warning and discards queued button cues.

The reed switch GPIO is a firmware board constant, not a BLE parameter:
`BOARD_REED_SWITCH_GPIO = GPIO_NUM_6`, active-low. The detector is one-shot per
wakeup, is not a Deep Sleep wake source, and only starts if the load is OFF.
Confirmed motion emits `WHEEL_MOTION_CONFIRMED`: the Load FSM runs the same
startup as a thermal-button press (GPIO ON, settling, fresh INA sample and
configured startup sound/error handling). Motion does not initialize BLE or
extend its advertising window.

## Configuration result payload

The result is five bytes:

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 1 | `version` | `uint8`, currently `6` |
| 1 | 2 | `request_id` | `uint16`, little-endian |
| 3 | 1 | `status` | `uint8`: `0` OK, `1` format, `2` range, `4` voltage ordering, `5` NVS/flash failure |
| 4 | 1 | `field` | `uint8`: `0` none, `1` idle current, `2` min voltage, `3` max voltage, `4` idle timeout, `5` button delta, `7` sleep interval, `8` buzzer threshold, `9` low delay, `10` button duration, `11` low duration, `12` combined duration, `13` buzzer enabled, `14` charging blink interval, `15` wheel minimum speed, `16` wheel circumference |

## Load state metric

The load-state characteristic remains one byte, both on reads and
notifications. Values are:

| Wire value | State |
|---:|---|
| 0 | `IDLE` |
| 1 | `WORKING` |
| 2 | `NEED_CHARGING` |

No notification payload size or layout changed. Existing clients must accept
the new state value `2`; it denotes an FSM state, not merely the physical GPIO
level (the load is kept ON during the NEED_CHARGING ON phase).

## Current format and examples

Firmware and client are updated together. Only v6 configuration blobs of exactly
34 bytes are accepted, both from BLE and from NVS. Other versions and lengths
are rejected with CONFIG_STATUS_FORMAT over BLE without changing settings.
Unsupported stored settings are ignored and factory defaults are used; no
migration or diameter conversion is performed. The next successful save writes
only the current format. Offset 32 always stores wheel circumference in mm.

The detector assumes one pulse per wheel revolution and uses
`max_interval_us = round(circumference_mm * 36000 / speed_deci_kmh)`.
Defaults 2220 mm and 100 deci-km/h give 799200 us. Circumference range is
900-10000 mm; its lower bound keeps the pulse interval above the existing
20-ms debounce at the maximum supported speed of 120 km/h.

Example v6 configuration write using request ID `0x1234` and all defaults:

```text
06 34 12 64 00 C8 19 D0 20 2C 01 00 00 14 00 88 13
58 1B 0A 00 F4 01 E8 03 DC 05 01 E8 03 64 00 AC 08
```

The payload is exactly 34 bytes. For example, offsets 28–33 encode the
charging interval `0x03E8 = 1000 ms`, minimum speed `0x0064 = 100` deci-km/h,
and wheel circumference `0x08AC = 2220 mm`.

Example successful result for request ID `0x1234`:

```text
06 34 12 00 00
```

### Client implementation checklist

1. Read and write the full 34-byte v6 blob; encode/decode all multibyte values
   little-endian and set byte 0 to `6`.
2. Subscribe to the configuration-result characteristic and correlate results
   by the echoed request ID; handle all documented status and field codes.
3. Reject unsupported versions and lengths. Do not select an older encoding
   based on the connected device.
4. Add UI controls for `charging_blink_interval_ms`,
   `wheel_min_speed_deci_kmh`, and `wheel_circumference_mm`, with the documented
   ranges and unit conversions.
5. Treat load state `2` as `NEED_CHARGING`; do not infer state solely from
   whether the physical load output is currently ON.
