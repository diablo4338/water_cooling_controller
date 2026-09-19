# Load controller firmware

The normal firmware path restores its RTC state and output pins, samples ADS1115
and INA226, advances the load state machine, applies a static DC output, and enters
deep sleep for 1 second. Boot and measurement time is additional to that second.
BLE is started only in the pairing/authorization service window.

## Current defaults

- Initial state after power-on, reset or invalid retained state: `idle`, load off.
- Button: logical temperature channel 4 (zero-based channel 3, ADS1115 input AIN2).
  Temperature strictly above 30 °C is pressed. Invalid ADC data is not a press or
  a release.
- `idle` + a new press: `working`, load at 100%.
- `working`: button presses do not change the state.
- Absolute INA current below 100 mA continuously for 300 seconds: `idle`, load off.
  A sample at/above the threshold or a failed INA read resets the timeout.
- After timeout, the button must be released and pressed again to restart.
- GPIO1 LED is on while the thermal button is pressed and off after release.
- Holding the thermal button for 10 seconds clears all trusted clients, disconnects
  the active client and opens pairing. The reset runs once per hold; another reset
  requires a release and a new hold.

Thresholds, timeout and the release requirement are defined in `policy` in
`firmware/src/ble_protocol/app/load_runtime.c`; the ADC input is defined alongside
it. These are provisional defaults. There is no BLE configuration or operation
interface; load policy is defined by the firmware.

## Retention and outputs

`load_state_t`, including the low-current start timestamp and previous button
state, is stored in RTC memory. System time (`gettimeofday`, RTC-backed in the
current ESP32-C3 configuration) includes deep sleep; the uptime timer does not.
No NVS writes are made per wakeup. Pairing credentials continue to use the existing
NVS storage and authorization protocol.

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
press is detected on the same ADS1115 channel and at the same 30 °C threshold as
the load command. Holding the temperature above the threshold does not keep
extending the window; another window requires a release and a new press.

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

Unity tests in `firmware/tests/ble_protocol/test_load_state.c` cover threshold
boundaries, ignored presses, retained timeout, invalid measurements and restart.
They are included in the existing `PAIR_RUN_TESTS` build.

Check GPIO3, GPIO10, GPIO2 and the GPIO1 LED with an oscilloscope across sleep entry, reset,
bootloader and application wakeup, both on and off. Verify no load interruption
in `working`, five-minute shutdown, sensor-disconnect behavior, ten-second trust
reset, metric read/notifications, and pairing and reauthorization through the service window. These electrical and BLE checks
require a connected board.
