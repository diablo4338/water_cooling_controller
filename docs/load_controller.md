# Load controller firmware

The normal firmware path restores its RTC state and output pins, samples ADS1115
and INA226, advances the load state machine, applies a static DC output, and enters
deep sleep for 1 second. Boot and measurement time is additional to that second.
BLE is started only in the pairing/authorization service window.

## Current defaults

- Initial state after power-on, reset or invalid retained state: `idle`, load off.
- Button: logical temperature channel 4 (zero-based channel 3, ADS1115 input AIN2).
  A press occurs when the current valid temperature is at least 2 °C above the
  median of the preceding five valid readings. Five readings are needed before
  the first press can be detected. Invalid ADC data leaves the button state and
  the history unchanged.
- `idle` + a new press: `working`, load at 100%.
- `working`: button presses do not change the state.
- Absolute INA current below 100 mA continuously for 300 seconds: `idle`, load off.
  A sample at/above the threshold or a failed INA read resets the timeout.
- After timeout, the button must be released and pressed again to restart.
- GPIO1 LED is on while the thermal button is pressed and off after release.
- GPIO0 buzzer sounds for 0.5 seconds on a new thermal-button press. If the INA
  bus voltage is below the buzzer threshold at that press, it sounds for 1.5 seconds.
  After more than 10 seconds of valid readings below the threshold, it sounds for
  1 second once per low-voltage episode. An invalid or recovered reading resets
  the low-voltage timer. A simultaneous press uses only the 1.5-second signal.
  A global enable flag silences all signals when disabled.
- Pressing the GPIO19 reset button clears all trusted clients, writes factory
  runtime parameters to NVS, disconnects the active client and opens pairing.
  The reset runs once while the button is held, including across deep sleep.

The temperature delta and other runtime settings are stored in NVS and configured
over BLE. The delta defaults to 2 °C and is encoded in 0.1 °C steps.
The battery display and runtime estimate use a factory voltage range of 6.6–8.4 V.
The client checks that values are numeric and fit their BLE fields. The firmware
checks the wire format, the buzzer's boolean flag, and that the upper battery
bound is strictly above the lower one. No other semantic ranges are imposed.
The BLE configuration format is version 4 (28 bytes); the client negotiates an
MTU large enough to read and write it. Existing version 1, 2 and 3 settings in
NVS are migrated. Version 3 buzzer settings are preserved and the new global
enable flag defaults to on; earlier versions receive factory buzzer settings.
Version 1 also receives the 2 °C button delta.
The buzzer threshold (default 7 V, 0 disables voltage alerts), low-voltage delay
(10 s), the three signal durations (0.5 / 1 / 1.5 s), and global buzzer enable
can be changed over BLE. Once the controller confirms disabling the buzzer, the
client displays its remaining buzzer settings as read-only.
The GPIO19 global reset restores the factory values for every runtime parameter,
including the 2 °C delta. The active parameters change after the NVS write succeeds.

## Retention and outputs

`load_state_t`, including the five-reading temperature history, low-current start
timestamp and previous button state, is stored in RTC memory. System time (`gettimeofday`, RTC-backed in the
current ESP32-C3 configuration) includes deep sleep; the uptime timer does not.
No NVS writes are made per wakeup. Pairing credentials continue to use the existing
NVS storage and authorization protocol.
The low-voltage timer and one-alert-per-episode flag are also retained in RTC memory.

| Output | Off (0%) | On (100%) |
| --- | --- | --- |
| GPIO3, high-side enable | 0 | 1 |
| GPIO10, inverted DC control | 1 | 0 |
| GPIO2, unused PWM control | 0 | 0 |

No LEDC/PWM is started by the new runtime. All three pins use per-pin hold and
deep-sleep hold. On wake, output latches and direction are restored before holds
are released. This follows the [ESP32-C3 GPIO hold requirements](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c3/api-reference/peripherals/gpio.html).
Retention covers deep sleep, not loss of power.

INA uses a fresh triggered shunt/bus conversion and polls the conversion-ready
flag with a bounded timeout, following the [INA226 datasheet](https://www.ti.com/lit/ds/symlink/ina226.pdf).
The existing ADC conversion and INA calibration are retained.

## BLE connection window

A new press of the temperature button opens BLE advertising for 60 seconds. The
same median-and-delta decision drives the load command and BLE window. The current
reading is added to the history after comparison; as the median catches up to a
warmer temperature, the button releases. Another window requires a new press.

If the trust store is empty, the connection window advertises the pairing service.
Otherwise it advertises the authorization service for an existing client.

During the window, the MCU stays awake and continues sampling/control approximately
once per second. A successful GAP connection blocks deep sleep for the complete
connection lifetime, including after the 60-second window expires. After disconnect,
the MCU keeps advertising only for any time left in the original window and then
returns to its sleep cycle. An authorized client can read the current controller
state, thermal-button state, temperature, INA bus voltage and INA current. Each value also supports
notifications. Notifications are published after every runtime sampling cycle
while subscribed; invalid sensor readings are represented as IEEE-754 `NaN`.
BLE connections cannot persist through deep sleep.

| Metric | Encoding |
| --- | --- |
| Controller state | `uint8`: 0 = idle, 1 = working |
| Thermal button | `uint8`: 0 = released, 1 = pressed |
| Temperature | little-endian `float32`, degrees Celsius |
| INA bus voltage | little-endian `float32`, V |
| Current | little-endian `float32`, mA |

## Validation on hardware

Unity tests in `firmware/tests/ble_protocol/test_load_state.c` cover the median
boundary, rolling history, retained timeout, invalid measurements and restart.
`firmware/tests/ble_protocol/test_runtime_config.c` verifies that factory
configuration is committed and reloaded from NVS.
`firmware/tests/ble_protocol/test_buzzer_logic.c` checks timing, invalid readings,
retention and combined-signal priority.
They are included in the existing `PAIR_RUN_TESTS` build.

Check GPIO3, GPIO10, GPIO2 and the GPIO1 LED with an oscilloscope across sleep entry, reset,
bootloader and application wakeup, both on and off. Verify no load interruption
in `working`, five-minute shutdown, sensor-disconnect behavior, ten-second trust
reset, metric read/notifications, and pairing and reauthorization through the service window. These electrical and BLE checks
require a connected board.
