# Firmware

The ESP-IDF project lives under `firmware/`.

- Components: `firmware/src/ble_protocol`, `firmware/src/metrics`, `firmware/src/i2c_bus`
- Firmware tests: `firmware/tests/`
- Build entry: `firmware/CMakeLists.txt`
- Active config: `firmware/sdkconfig`

## Runtime and diagnostics

- [Settings reference, firmware constants, and client environment variables](settings.md)
- [Runtime behavior](runtime.md) and [state-machine transition tables](state-machines.md)
- Firmware diagnostics use ESP-IDF logging on the serial console. From `firmware/`, run `idf.py -p COMx monitor` with the controller's port. Listen during the operation or save the console output while reproducing it.
- For overloads, capture the `output_limit` tag. Transition lines include the event, phase duration, applied output, active cap, current, conversion sequence, averaging-window start bound, and threshold. Probe and applied-output lines show the requested and applied percentages.
- In the overload log, `window_after` is the lower bound for the averaging window start and `applied_at` is the last output change. Both timestamps are milliseconds since boot. The sequence identifies the cached conversion used in the decision. Repeated upward probes can appear as `search_up -> search_up`.
- The desktop app shows live operation progress and terminal results under **Operations (log)**. **Debug log** shows diagnostic messages when debug display is enabled. **Real-time data** shows incoming measurements. These three views live in the current app session.
- The desktop app persists chart history separately in `platformdirs.user_config_dir("BLECoolingController")/metrics_history/<device-address>.json`. The sanitized address identifies the controller. [Storage implementation](../pcwcc/src/pcwcc/core_logic/storage.py)
