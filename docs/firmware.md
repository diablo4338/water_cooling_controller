# Firmware

The ESP-IDF project lives under `firmware/`.

- Components: `firmware/src/ble_protocol`, `firmware/src/metrics`, `firmware/src/i2c_bus`
- Firmware tests: `firmware/tests/`
- Build entry: `firmware/CMakeLists.txt`
- Active config: `firmware/sdkconfig`

The firmware implements the [load controller](load_controller.md): an RTC-retained
idle/working state machine, synchronous ADS1115/INA226 reads, static DC outputs
and a one-second deep-sleep cycle. BLE exposes pairing and authorization only.
The metrics component contains the two sensor adapters, without background
sampling, tachometry or telemetry aggregation.
