# Controller settings reference

This reference covers the ESP32-C3 water cooling controller and its PCWCC desktop client.

| What to change | Where to set it | When it takes effect |
| --- | --- | --- |
| Mode, working minimum, temperature thresholds, and fan monitoring | Edit the fields in PCWCC and select **Apply** | After the controller confirms the save |
| Global output ceiling | **Maximum output** → **Save maximum** | After the controller confirms the save |
| Temporary ceiling set by protection | The firmware calculates it. **Reset overload** clears the temporary ceiling and overload fault with a valid current conversion at the applied output within threshold | For the current boot session. A reboot restores the saved maximum |
| Current threshold, sampling delays, protection steps, and GPIO assignments | Constants and literals in `firmware/src/` | After rebuilding and flashing the firmware |
| Client BLE timeouts and connection options | PCWCC process environment variables | When the client starts |
| ESP-IDF build settings | `firmware/sdkconfig`, `firmware/sdkconfig.defaults`, or `idf.py -C firmware menuconfig` | After rebuilding and flashing the firmware |

C names ending in `_US` take integer microseconds, `_MS` takes milliseconds, and `_HZ` takes hertz. For example, 3.5 seconds for `CURRENT_SAMPLE_MAX_AGE_US` is expressed as `3500000LL`. Fractional C `float` values use a decimal point, as in `5.0f`. FreeRTOS delay granularity depends on the tick frequency.

## Settings saved by the controller

| Client field | Firmware field | Factory default | Format and behavior |
| --- | --- | --- | --- |
| **Base fan speed, %** | `fan_min_speed` | 50% | Integer input from 10 to 100. The controller caps the saved value at the global maximum on Apply. It sets the requested speed in Always on mode and the regulator's lower bound while temperature control is on |
| **Maximum output** | `maximum` in the configuration record | 100% | Integer from 10 to 100. Caps output in all modes, including service operations. Saving a ceiling below the working minimum lowers the minimum in the same write |
| **Control type** | `fan_control_type` | DC | `0 = DC`, `1 = PWM` |
| **Mode** | `fan_mode` | Always on | `0 = Always on`, `1 = By temperature sensor`, `2 = Inactive`. Temperature mode requires an available control thermistor |
| **Start temperature** | `fan_start_temp` | 35 °C | Integer in °C, strictly greater than the shutdown delta and below the maximum temperature |
| **Max temperature** | `fan_max_temp` | 45 °C | Integer up to 150 °C, strictly greater than the start temperature. At this threshold, the temperature regulator requests 100% and thermal protection sets the overheat flag. Actual output remains subject to the ceiling |
| **Shutdown delta** | `fan_off_delta` | 2 °C | Integer from 0 to `fan_start_temp − 1`. With a positive delta, the output stops at or below `fan_start_temp − fan_off_delta`. The factory shutdown threshold is 33 °C. With a zero delta, the start condition takes priority at the shared threshold |
| **Monitor fan 1 faults** | `fan_monitoring_enabled` | Enabled | Fixed at `1`. The firmware always saves monitoring enabled for the first channel |
| **Monitor fan 2/3/4 faults** | `fan2_monitoring_enabled` through `fan4_monitoring_enabled` | All enabled | Checkboxes encoded as `0` or `1`. Enable stall detection on each channel |
| **Overheat alarm** | `overheat_alarm_enabled` | Enabled | `0` or `1`. Controls the overheat buzzer. Temperature checks and overcurrent protection remain active according to their own rules |

Factory defaults come from `PARAMS_DEFAULTS_INIT` and `PARAMS_OUTPUT_MAX_DEFAULT`. The controller stores its settings in the `params/config` NVS record. The firmware updates a single RAM snapshot after a successful write. Changes to C defaults apply to empty storage or after a factory reset. During active overload adjustment, **Apply**, **Apply profile**, and **Save maximum** return `OUTPUT_ADJUSTING`. Settings can still be saved during a latched fault. Output can resume after a separate fault reset or reboot. [Defaults and validation](../firmware/src/ble_protocol/control/params.c) · [Client fields](../pcwcc/src/pcwcc/gui_logic/constants.py) · [Buzzer control](../firmware/src/ble_protocol/hw/overheat_alarm.c)

### BLE format and profiles

- **The v6 parameter packet** is 26 bytes, little-endian, with Python format `<BHiBiiiBBBBBB`. It contains a version byte, a two-byte field mask, the minimum as `int32`, the control type as `uint8`, the maximum temperature, shutdown delta, and start threshold as `int32` values, then the mode and five flags as `uint8` values. The all-fields mask is `0x07FF`. Bits correspond to the `PARAM_FIELD_*` IDs in the firmware header. Writing the packet creates a draft. Applying it sends one byte, `0x01`, to the status characteristic. [Field layout](../firmware/src/ble_protocol/control/params.h) · [Client codec](../pcwcc/src/pcwcc/core_logic/codec.py)
- **The global maximum** uses the three-byte command `[1, 1, maximum]`. For example, 80% is `01 01 50` in hex. **Reset overload** sends `[1, 2, 0]` on the same characteristic (`…e19`). Its six-byte status v2 returns the version, saved ceiling, effective ceiling, state, reset sequence, and reset result. The separate read/notify characteristic (`…e1c`) uses an eight-byte v3 status. It reports the confirmed ceiling during adjustment and adds the temporary minimum (`0xFF` if absent) and overload-without-load flag. A zero saved maximum in status signals a settings read error and blocked output. The maximum write command accepts 10–100%. [Commands](../firmware/src/ble_protocol/ble/gatt.c) · [Status encoding](../firmware/src/ble_protocol/ble/fan_status_ble.c)
- **Profiles** are stored under `profiles/p1`–`p3`. The user selects slot 1–3. Titles and descriptions are stored under `profiles/m1`–`m3`, with limits of 32 and 96 **UTF-8 bytes**, respectively. A profile stores the parameter packet. The global maximum applies to the whole controller. Factory reset preserves profiles. [Profile storage](../firmware/src/ble_protocol/control/profiles.c) · [Profile limits](../firmware/src/ble_protocol/control/profiles.h)

## Overcurrent protection and INA226

| Constant or source | Value | What it controls |
| --- | --- | --- |
| `INA226_OVERCURRENT_THRESHOLD_MA` | 100 mA in this source tree | Firmware constant in `ina226.h`. Current above this value triggers protection. Current at or below it can verify a step or permit reset |
| `CAL_VALUE`, `INA226_CURRENT_LSB_UA`, `ina226_current_raw_to_ma()` | 2179 (`0x0883`), 50 µA per count, `0.05f` mA multiplier | Current readout calibration. The driver currently uses the `0.05f` literal, so changing `INA226_CURRENT_LSB_UA` alone leaves the readout scale unchanged |
| `INA226_CONFIG_VALUE` | `0x4D37` | Continuous shunt and bus conversion with 512-sample averaging. Nominal conversion times are 4.156 ms for the shunt and 1.1 ms for the bus. One averaged batch takes about 2.69 s |
| `INA226_MASK_ENABLE_VALUE` | `0x0000` | Disables the hardware ALERT pin. The conversion-ready flag is available through I²C |
| `INA226_ADDR`, `INA226_I2C_FREQ_HZ`, `INA226_I2C_TIMEOUT_MS` | `0x40`, 400,000 Hz, 100 ms | 7-bit address, SCL frequency, and access timeout |
| `INA226_POLL_PERIOD_MS` | 500 ms | Poll interval for the INA226 monitor task. It checks the ready flag and publishes a new sample only after a completed conversion |
| `CURRENT_SAMPLE_MAX_AGE_US` | 3,500,000 µs | Maximum age of a completed current measurement used by protection, including room for the 500 ms polling interval |
| `CURRENT_MEASUREMENT_TIMEOUT_US` | 12,000,000 µs | Maximum time to obtain a valid current window for the applied output after `MEASURE` starts |
| `SEARCH_STEP_PERCENT` | `5.0f` percentage points | Step size for the upward search and the single rollback after a fresh conversion exceeds the threshold |
| `VERIFY_TIMEOUT_US` | 9,000,000 µs | Maximum wait for a configuration write, a completed probe measurement, or applied fallback feedback |

Current protection consumes the same averaged readings as telemetry. After an applied output change the limiter requires a complete averaging window at the new output. It advances on measurement availability. [Threshold and snapshot](../firmware/src/metrics/ina226.h) · [INA226 driver](../firmware/src/metrics/ina226.c) · [Output limiter](../firmware/src/ble_protocol/control/output_limit.c)

## Temperature, I²C, and polling intervals

| Constant or source | Value | What it controls |
| --- | --- | --- |
| `METRICS_POLL_TASK_PERIOD_MS` | 250 ms | Delay **after** polling ADS1115. A full cycle also includes conversion and I²C access time |
| `ADS1115_CONV_DELAY_MS` | 10 ms | Delay before reading a single-shot conversion. Each channel takes two conversions and discards the first |
| `ADS1115_INTER_CH_DELAY_MS` | 5 ms | Requested delay between channels. At 100 Hz, `pdMS_TO_TICKS(5)` yields zero ticks |
| `ADS1115_I2C_TIMEOUT_MS`, `ADS1115_RETRY_COOLDOWN_US` | 100 ms, 1,000,000 µs | I²C timeout and cooldown after a transfer error |
| `ADS1115_I2C_ADDR`, `ADS1115_I2C_FREQ_HZ` | `0x48`, 400,000 Hz | 7-bit sensor address and bus frequency |
| `ads1115_build_config()`, `ADS1115_FS_V` | 128 SPS, single-ended inputs, single-shot conversion, ±4.096 V range | ADC configuration. If the gain changes, update both the register setting and the conversion scale |
| `ads1115_raw_to_temp()` | 3.3 V, 4,700 Ω, 10 kΩ NTC at 25 °C, B = 3,625 K | Divider and thermistor values. An input at or below 1 µV or a nonpositive calculated resistance is invalid |
| `METRICS_RAW_NO_SENSOR_THRESHOLD` | 500 ADS counts | A lower reading is treated as a missing sensor |
| `METRICS_FAIL_THRESHOLD`, `ADC_RECOVERY_BATCHES` | 3, 3 | Number of channel errors and number of successful cycles required for ADC recovery |
| `METRICS_TEMP_CHANNELS`, `METRICS_CONTROL_TEMP_CHANNEL`, `g_ads_input_by_temp_channel[]` | 4, index 3, `{1, 0, 3, 2}` | Number of channels, control channel **Temp4**, and ADS input mapping. Temp4 reads AIN2 |
| `THERMAL_EMERGENCY_THRESHOLD_C` | 50 °C | A valid control temperature strictly above this threshold raises overheat in every mode |
| `THERMAL_SAMPLE_MAX_AGE_US` | 3,000,000 µs | Maximum temperature reading age for the regulator and overheat check |
| `SHARED_I2C_PORT`, `SHARED_I2C_SDA_GPIO`, `SHARED_I2C_SCL_GPIO` | I²C 0, GPIO 5, GPIO 4 | Shared ADS1115 and INA226 bus |
| `shared_i2c_bus_init()` | `glitch_ignore_cnt = 7`, internal pull-ups enabled | Short-pulse filtering and bus pull-up configuration |

The driver rounds temperature to 0.01 °C. Published readings are rounded to 0.1 °C. `TEMP_NOTIFY_EPS` is 0.01 °C. The voltage and current notification thresholds in the BLE metrics publisher are 0.01 V and 0.1 mA. The `METRICS_TASK_PERIOD_MS` publisher targets 250 ms and sends updates on changes, including an output change of at least 0.5 percentage points. [ADS1115 driver](../firmware/src/metrics/ads1115.c) · [ADS timing](../firmware/src/metrics/ads1115.h) · [Measurements and filters](../firmware/src/metrics/metrics.c) · [Channels](../firmware/src/metrics/metrics.h) · [Temperature protection](../firmware/src/ble_protocol/control/thermal_protection.c) · [I²C bus](../firmware/src/i2c_bus/shared_i2c_bus.h) · [Bus configuration](../firmware/src/i2c_bus/shared_i2c_bus.c) · [Publishing](../firmware/src/ble_protocol/ble/metrics_ble.c)

## Fan output and tachometers

| Constant or source | Value | What it controls |
| --- | --- | --- |
| `fan_control_task()` | 200 ms delay | Delay between output calculation, protection checks, and operation steps |
| `FAN_HIGHSIDE_GPIO`, `FAN_PWM_GPIO_DC`, `FAN_PWM_GPIO_PWM` | GPIO 3, 10, 2 | High-side switch and control outputs. The switch is active-high |
| `FAN_DC_FREQ_HZ`, `FAN_PWM_FREQ_HZ` | 1,000 and 25,000 Hz | Signal frequencies in DC and PWM modes |
| `FAN_DC_RES_BITS`, `FAN_PWM_RES_BITS` | 10 bits for both | LEDC resolution. Both use low-speed mode, timer 0, and channel 0 |
| `METRICS_FAN_CHANNELS`, `g_tach_select_by_fan_channel[]` | 4, `{0, 2, 3, 1}` | Number of tachometers and multiplexer selection order |
| `FAN_TACH_GPIO`, `FAN_TACH_SEL_GPIO0/1` | GPIO 18, 6, 7 | Tachometer input and multiplexer select pins |
| `FAN_TACH_PULSES_PER_REV` | 2 | Pulses per revolution for RPM conversion |
| `FAN_TACH_MIN_VALID_DT_US`, `FAN_TACH_MAX_VALID_DT_US`, `FAN_TACH_MIN_LOW_WIDTH_US` | 10,000, 250,000, and 50 µs | Accepted interval between edges (120–3,000 RPM at two pulses per revolution) and minimum low pulse width |
| `FAN_TACH_DT_BUF_SIZE`, `FAN_TACH_MIN_CALC_SAMPLES`, `FAN_TACH_TRIM_SAMPLES` | 16, 2, 0 | Period buffer size, minimum samples for calculation, and the number of samples trimmed from each end before the median |
| `FAN_RPM_EMA_OLD`, `FAN_RPM_EMA_NEW`, `FAN_RPM_NOTIFY_EPS` | `0.2f`, `0.8f`, 1 RPM | RPM filter weights and notification threshold |
| `METRICS_TACH_CHANNEL_WINDOW_MS`, `METRICS_TACH_SCAN_US`, `FAN_TACH_SWITCH_DELAY_US` | 800 ms, approximately 3.2 s, 1,000 µs | Time per channel, nominal scan time across all four channels, and the interval during which edges are ignored after switching |
| `FAN_TACH_STOP_TIMEOUT_US`, `FAN_START_TIMEOUT_US`, `FAN_FAIL_TIMEOUT_US` | 4 s, 7.2 s, and 4 s with four channels | No-RPM detection, startup timeout, and RPM-loss timeout |

`fan_control_apply_output()` uses inverted duty to convert the output percentage to the board's hardware signal. If the pinout, switch polarity, or frequency changes, check the corresponding driver branch. [Fan output](../firmware/src/ble_protocol/control/fan_control.c) · [Tachometers](../firmware/src/metrics/metrics.c)

## Service operations

| Constant | Value | What it controls |
| --- | --- | --- |
| `PREFLIGHT_CONFIRM_US`, `PREFLIGHT_TIMEOUT_US` | 3 s, 45 s | Minimum settling time and timeout for each DC or PWM test |
| `OP_CALIB_START_SPEED`, `OP_CALIB_STEP_DELTA` | 10%, 5 percentage points | Starting output and increment per calibration step |
| `OP_CALIB_MAX_SPEED_OFFSET` | `30.0f` percentage points | Subtracted from the preflight ceiling to get the upper bound of the search. The upper bound is at least the starting speed |
| `OP_CALIB_STOP_WAIT_US`, `OP_CALIB_BASELINE_WAIT_US` | 6 s, 4 s | Wait before startup and before reading baseline RPM |
| `OP_CALIB_INCREMENT_PAUSE_US`, `OP_CALIB_STEP_WAIT_US` | 3 s, 4 s | Pause after zero output is applied and settling time after the new step is applied |
| `OP_CALIB_ROTATION_WINDOWS`, `OP_CALIB_OBSERVATION_MAX_AGE_US`, `OP_CALIB_ROTATION_TIMEOUT_US` | 3 windows, 4 s, 13.6 s | Required consecutive tachometer windows per monitored channel, maximum window age or gap, and the confirmation deadline after settling |
| `OP_CALIB_RPM_DELTA` | `50.0f` RPM | Required RPM increase on the first channel relative to the baseline |
| `OP_SETUP_WAIT_US`, `OP_SETUP_PWM_TARGET_OFFSET`, `OP_SETUP_RPM_DROP` | 6 s, `30.0f` percentage points, `50.0f` RPM | DC/PWM settling time, reduction from the available PWM ceiling, and required drop in RPM |
| `OP_OUTPUT_CONFIRM_TIMEOUT_US` | 9 s | Final output confirmation timeout. The timer restarts after waiting for overload adjustment |

Edit the `_US` constants in microseconds, even when the table shows seconds. [Preflight](../firmware/src/ble_protocol/control/operation_preflight.c) · [Calibration and automatic setup](../firmware/src/ble_protocol/control/operation_manager.c)

## Firmware indicators, button, and BLE

| Constant or source | Value | What it controls |
| --- | --- | --- |
| `STATUS_LED_GPIO`, `STATUS_LED_ON_LEVEL/OFF_LEVEL` | GPIO 1, 1/0 | Status LED and polarity |
| `STATUS_LED_POLL_MS`, `STATUS_LED_SLOW_BLINK_MS`, `STATUS_LED_FAST_BLINK_MS` | 50, 2,000, and 120 ms | Polling interval and slow and fast LED **toggle** intervals |
| `STATUS_LED_FACTORY_BLINK_MS`, `STATUS_LED_FACTORY_SUCCESS_TOGGLES`, `STATUS_LED_FACTORY_FAILURE_TOGGLES` | 150 ms, 4 and 6 toggles | Factory reset result indication |
| `OVERHEAT_ALARM_GPIO`, `OVERHEAT_ALARM_ON_LEVEL/OFF_LEVEL` | GPIO 0, 1/0 | Buzzer and polarity |
| `OVERHEAT_ALARM_POLL_MS`, `OVERHEAT_ALARM_PHASE_MS` | 20 and 2,000 ms | Polling interval and duration of each sound or silence phase |
| `PAIR_BTN_GPIO`, `button_task()` | GPIO 19, 20 ms polling | Active-low button. Holding it for more than 3,000 ms opens pairing. Holding it for more than 10,000 ms starts a factory reset |
| `PAIR_MODE_TIMEOUT_US` | 60 s | Pairing window duration |
| `gatt.c`, pairing completion timer | 250,000 µs | Delay before disconnecting after successful pairing |
| `app_main()`, factory reset recovery | 1,000 ms | Delay between attempts to finish an interrupted reset |
| `TRUST_MAX_CLIENTS` | 5 | Trust store capacity |
| `start_advertising()` | `sensor`, `sensor-pair` | BLE scan response names. NimBLE selects the advertising intervals |

Protocol headers define UUIDs, state IDs, and packet versions. Changing them requires coordinating both ends of the BLE connection. [LED](../firmware/src/ble_protocol/hw/status_led.c) · [Buzzer](../firmware/src/ble_protocol/hw/overheat_alarm.c) · [Button](../firmware/src/ble_protocol/app/app_tasks.c) · [Button GPIO](../firmware/src/ble_protocol/pairing/state.h) · [Pairing window](../firmware/src/ble_protocol/pairing/pair_mode.c) · [GATT](../firmware/src/ble_protocol/ble/gatt.c) · [Advertising](../firmware/src/ble_protocol/ble/gap.c) · [Trust store capacity](../firmware/src/ble_protocol/pairing/storage.h)

## PCWCC client

Set environment variables before starting the process. In PowerShell, run `$env:BLE_CONNECT_TIMEOUT_S = '15.0'` and start PCWCC from the same session. Seconds use a decimal point. True values are `1`, `true`, `yes`, and `on`, case-insensitively. False values are `0`, `false`, `no`, and `off`. Empty or invalid numeric values fall back to the default.

| Variable | Default | Purpose |
| --- | --- | --- |
| `BLE_SCAN_TIMEOUT_S` | 10.0 s | Scan timeout |
| `BLE_RESOLVE_TIMEOUT_S` | 10.0 s | Time to resolve a BLE device before connecting |
| `BLE_CONNECT_TIMEOUT_S` | 10.0 s | BLE connection timeout |
| `BLE_PAIR_TIMEOUT_S` | 8.0 s | Client pairing timeout |
| `BLE_AUTH_TIMEOUT_S` | 5.0 s | Authorization timeout |
| `BLE_METRICS_TIMEOUT_S` | 5.0 s | Read, write, and telemetry timeout |
| `BLE_METRICS_RETRIES` | 1 | Integer number of telemetry read retries after a disconnect |
| `BLE_METRICS_RECONNECT_DELAY_S` | 0.3 s | Delay when reconnecting to read telemetry |
| `GUI_ACTION_DEFAULT_TIMEOUT_S` | 5.0 s | General GUI action timeout |
| `GUI_ACTION_SCAN_TIMEOUT_S` | 10.0 s | GUI scan action timeout |
| `GUI_ACTION_PAIR_TIMEOUT_S` | From `BLE_PAIR_TIMEOUT_S` | GUI pairing action timeout |
| `GUI_ACTION_CONNECT_TIMEOUT_S` | From `BLE_CONNECT_TIMEOUT_S` | GUI connect action timeout |
| `GUI_ACTION_DISCONNECT_TIMEOUT_S` | 5.0 s | GUI disconnect action timeout |
| `BLE_RESOLVE_BEFORE_CONNECT` | True on Windows, false elsewhere | Resolve the device before connecting |
| `BLE_USE_SERVICE_FILTER` | False on Windows, true elsewhere | Pass a BLE service filter when connecting |
| `BLE_WINRT_USE_CACHED_SERVICES` | False | Use the Windows GATT service cache |
| `BLE_WINRT_ADDRESS_TYPE` | Empty | WinRT address type string, such as `public` or `random`. An empty value leaves the choice to the backend |

- **Local client settings.** Auto-connect is stored as `auto_connect` for the selected device in `paired_devices.json`. It defaults to off, and at most one controller can be selected. The `--debug` flag enables the log panel for the current run, for example `pcwcc --debug`. [Local storage](../pcwcc/src/pcwcc/core_logic/storage.py) · [GUI startup](../pcwcc/src/pcwcc/gui_logic/app.py)
- **BLE status.** The client subscribes to fan mode and limiter status, then reads each initial value once. The controller sends changes as notifications. **Reset overload** waits for a terminal notification with the expected sequence number. Link loss is delivered through the BLE backend's disconnect callback. [Subscriptions](../pcwcc/src/pcwcc/gui_logic/worker_flows.py) · [Protection reset](../pcwcc/src/pcwcc/core_logic/control.py)
- **Chart history.** `METRICS_HISTORY_WINDOW_SECONDS` and `WINDOW_SECONDS` are set independently and both default to 30 minutes. `SERIES_GAP_SECONDS=30.0` determines how large a time gap breaks a plotted line. [History storage](../pcwcc/src/pcwcc/core_logic/storage.py) · [Chart](../pcwcc/src/pcwcc/gui_logic/metrics_chart.py)

## Build and test setup

- **ESP-IDF.** The active `firmware/sdkconfig` targets ESP32-C3 and sets `CONFIG_FREERTOS_HZ=100`, WARN logging, UART at 115200 baud, and the single app large partition table. `sdkconfig.defaults` supplies initial values for a fresh configuration. Use `-DSDKCONFIG=<path>` to build with a separate file. `-DPAIR_RUN_TESTS=1` enables firmware tests. [Active configuration](../firmware/sdkconfig) · [Build defaults](../firmware/sdkconfig.defaults) · [Test flag](../firmware/src/ble_protocol/CMakeLists.txt)
- **ESP-IDF tasks.** In `app_main()`, the BLE metrics, fan, and button tasks use priority 5 and a 4,096-byte stack. The LED and buzzer tasks use priority 3 and a 2,048-byte stack. The measurement and tachometer tasks use `METRICS_TASK_PRIORITY=5` and `METRICS_TASK_STACK_SIZE=4096`. [Task creation](../firmware/src/ble_protocol/app/main.c) · [Sensor tasks](../firmware/src/metrics/metrics.c)
- **Build scripts.** The `Makefile` accepts `IDF_EXPORT`, `IDF_PY`, and `IDF_DIR` for firmware builds, and `PYTHON`, `CLI_PYTHONPATH`, and `APP_ARGS` for starting the client. The firmware release script takes the version as its first argument and reads `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_TARGET`, and `IDF_CCACHE_ENABLE`. [Makefile](../Makefile) · [Release script](../scripts/build_firmware.sh)
- **Integration tests.** `BLE_ADDRESS` specifies the controller address and is required in the environment for `--runintegration` and `--runmanual`. `BLE_ADAPTER` selects the adapter, for example `hci0`. `SCAN_TIMEOUT_S=8.0` and `CONNECT_TIMEOUT_S=20.0` apply to tests. The button fixture uses `PRESS_BASE_URL` (default `http://192.168.1.60:8001`), `PRESS_TIMEOUT_S=10.0`, `PRESS_RETRIES=2`, and `PRESS_NO_RESPONSE=0`. [Test launch settings](../pcwcc/tests/conftest.py)
