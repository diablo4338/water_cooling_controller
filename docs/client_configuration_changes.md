# BLE client configuration changes

This document describes the implemented firmware protocol in configuration
version 5. The main BLE service UUID is
`0814951d-ca1b-4581-b1af-9c4f2ca96e90`.

## Characteristics

| Purpose | UUID | Properties | Payload |
|---|---|---|---|
| Runtime configuration | `dc881986-aada-4764-bfb9-d79326a6c03a` | Read, Write | Versioned configuration blob, 34 bytes in v5 |
| Configuration result | `11d1283c-ad34-406d-9d96-7a11c69db086` | Read, Notify | 5-byte result |
| Load state metric | `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718281` | Read, Notify | One-byte state value |

Configuration access requires the existing authenticated data-access
permission. Write the entire configuration blob; individual fields are not
written separately. A successful write is persisted to NVS before the active
configuration changes. The result characteristic also sends a notification
after each write.

## Configuration version and wire layout

Configuration v4 was 28 bytes. Configuration v5 is 34 bytes. All multibyte
integers are unsigned and little-endian. Offsets are zero-based.

| Offset | Size | Field / header | Type | v5 behavior |
|---:|---:|---|---|---|
| 0 | 1 | `version` | `uint8` | Must be `5` for v5 writes |
| 1 | 2 | `request_id` | `uint16` | Client correlation ID; echoed in the result |
| 3 | 2 | `idle_current_ma` | `uint16` | Existing field; idle and charging current threshold |
| 5 | 2 | `voltage_min_mv` | `uint16` | Existing field; low-voltage threshold used for NEED_CHARGING |
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
| 28 | 2 | `charging_blink_interval_ms` | `uint16` | New in v5; duration of each ON and OFF phase |
| 30 | 2 | `wheel_min_speed_deci_kmh` | `uint16` | New in v5; speed stored in 0.1 km/h units |
| 32 | 2 | `wheel_diameter_mm` | `uint16` | New in v5 |

### Configuration parameters

Every listed configuration field is stored together in the `runtime_cfg/values`
NVS blob and is accessible through the runtime-configuration characteristic.
Every field is writable as part of the full configuration blob and readable
from the same characteristic. The firmware does not require a restart for the
charging blink interval or thresholds; wheel speed and diameter changes are
used by the reed detector on the next startup/wakeup cycle.

Thus, for each field in the table: NVS = yes; BLE = yes; access = readable and
writable; restart = no, except `wheel_min_speed_deci_kmh` and
`wheel_diameter_mm`, which are picked up by the detector on the next
startup/wakeup cycle (not immediately in the current cycle).

For legacy fields whose validation is type-only, the accepted minimum and
maximum below are the full unsigned representation range. `voltage_max_mv` must
also be greater than `voltage_min_mv`.

| Exact field name | Description and units | Type / size | Default | Accepted values | Suggested UI |
|---|---|---:|---:|---|---|
| `idle_current_ma` | Low-current threshold, mA; also used by NEED_CHARGING confirmation and blink measurement | `uint16`, 2 bytes | 100 | 0–65535 | “Low current threshold”, numeric mA input |
| `voltage_min_mv` | Low-voltage threshold, mV; battery voltage must be below it for NEED_CHARGING | `uint16`, 2 bytes | 6600 | 0–65535; must be less than `voltage_max_mv` | “Low battery threshold”, numeric mV input |
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
| `charging_blink_interval_ms` | Duration of each NEED_CHARGING ON phase and each OFF phase, ms | `uint16`, 2 bytes | 5000 | 1000–60000 | “Charging blink interval”, numeric ms input |
| `wheel_min_speed_deci_kmh` | Minimum speed to confirm motion, in 0.1 km/h (50 means 5.0 km/h) | `uint16`, 2 bytes | 50 | 10–1200 (1.0–120.0 km/h) | “Minimum wheel speed”, decimal km/h input converted to tenths |
| `wheel_diameter_mm` | Wheel diameter used for one-revolution timing, mm | `uint16`, 2 bytes | 2100 | 300–3000 | “Wheel diameter”, numeric mm input |

The NEED_CHARGING entry thresholds are deliberately the existing
`voltage_min_mv` and `idle_current_ma` settings; there are no duplicate
threshold fields. Low voltage and low current must both be confirmed for five
seconds. The current condition uses absolute measured current and is strictly
below the threshold. Invalid or missing measurements do not count as low.

The reed switch GPIO is a firmware board constant, not a BLE parameter:
`BOARD_REED_SWITCH_GPIO = GPIO_NUM_6`, active-low. The detector is one-shot per
wakeup, is not a Deep Sleep wake source, and only starts if the load is OFF.

## Configuration result payload

The result is five bytes:

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 1 | `version` | `uint8`, currently `5` |
| 1 | 2 | `request_id` | `uint16`, little-endian |
| 3 | 1 | `status` | `uint8`: `0` OK, `1` format, `2` range, `4` voltage ordering, `5` NVS/flash failure |
| 4 | 1 | `field` | `uint8`: `0` none, `1` idle current, `2` min voltage, `3` max voltage, `4` idle timeout, `5` button delta, `7` sleep interval, `8` buzzer threshold, `9` low delay, `10` button duration, `11` low duration, `12` combined duration, `13` buzzer enabled, `14` charging blink interval, `15` wheel minimum speed, `16` wheel diameter |

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

## Compatibility, migration, and examples

NVS migration accepts stored v1–v4 configurations. Existing values are
preserved; v5-only fields receive defaults (`5000 ms`, `50` deci-km/h, and
`2100 mm`). The next successful configuration write persists the complete v5
blob.

For compatibility, the firmware also accepts a 28-byte v4 configuration
write, preserving the current v5-only values. It still returns the current
five-byte v5 result, and configuration reads return the 34-byte v5 layout.
Therefore v4 clients that only write their existing 28-byte blob can continue
to do so, but clients must be updated before they read or parse configuration
responses as v4. Clients should not assume that a successful v4 write changes
the new v5-only settings.

Example v5 configuration write using request ID `0x1234` and all defaults:

```text
05 34 12 64 00 C8 19 D0 20 2C 01 00 00 14 00 88 13
58 1B 0A 00 F4 01 E8 03 DC 05 01 88 13 32 00 34 08
```

The payload is exactly 34 bytes. For example, offsets 28–33 encode the
charging interval `0x1388 = 5000 ms`, minimum speed `0x0032 = 50` deci-km/h,
and wheel diameter `0x0834 = 2100 mm`.

Example successful result for request ID `0x1234`:

```text
05 34 12 00 00
```

### Client implementation checklist

1. Read and write the full 34-byte v5 blob; encode/decode all multibyte values
   little-endian and set byte 0 to `5`.
2. Subscribe to the configuration-result characteristic and correlate results
   by the echoed request ID; handle all documented status and field codes.
3. Keep the existing 28-byte v4 write path only if compatibility with older
   firmware/clients is needed. Do not parse v5 reads as v4.
4. Add UI controls for `charging_blink_interval_ms`,
   `wheel_min_speed_deci_kmh`, and `wheel_diameter_mm`, with the documented
   ranges and unit conversions.
5. Treat load state `2` as `NEED_CHARGING`; do not infer state solely from
   whether the physical load output is currently ON.
