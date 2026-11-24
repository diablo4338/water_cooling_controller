# Controller runtime

This document describes the firmware and PCWCC behavior in the current source tree. The [state-machine reference](state-machines.md) lists the states, event producers, transition guards, and recipients. The [settings reference](settings.md) gives defaults, units, and source locations. The [design milestones](design-milestones.md) records how these policies evolved.

## Runtime ownership

The fan task runs with a 200 ms delay between iterations. Each iteration reads committed configuration, updates thermal protection, advances the operation manager, and selects normal or service demand. The output limiter converts that demand into an output percentage and DC/PWM command. After a successful hardware write, the fan task reports what was applied and the operation manager checks the result. A driver failure sets `DEVICE_ERROR_FAN_OUTPUT` and the driver attempts to cut power. The fan task publishes runtime mode, limiter, and tachometer status through the BLE transport. [Fan task and driver](../firmware/src/ble_protocol/control/fan_control.c)

## Operating modes

| Mode | Demand before overcurrent protection |
| --- | --- |
| **Always on** | The saved **Base fan speed, %** is the requested percentage. Temperature readings remain visible and do not affect normal fan output. The emergency overheat threshold still applies |
| **By temperature sensor** | At the start threshold, the fan starts at the base speed. Demand rises linearly toward 100% at the maximum temperature. While running between the stop and start thresholds, demand is the base speed. At or below start minus shutdown delta, demand becomes 0% |
| **Inactive** | Requests 0%. A change to this mode generates a stop request when the fan task observes it |
| **Service** | A calibration or automatic setup step supplies the desired percentage and DC/PWM type through the fan override. The client shows Service as the effective mode |

The persisted configuration owns the selected mode. The fan task derives the effective mode, applies the shared limiter, and reports both modes and temperature input status. In temperature mode, the effective ceiling also caps the low end of running demand. At the temperature maximum, the regulator requests 100%, subject to the active ceiling. [Fan control](../firmware/src/ble_protocol/control/fan_control.c)

## Temperature input and overheat

The ADS1115 poller supplies readings and I²C health. It requires three successful batches at startup or after a bus failure. The controller uses zero-based channel 3. A missing, non-finite, or more than three seconds old control measurement gives `NTC_UNAVAILABLE` when the ADC is available. ADC I²C failure and unavailable control thermistor have separate device diagnostics. Missing readings can be displayed as unavailable while the remaining channels and BLE session continue running. [ADC polling](../firmware/src/metrics/metrics.c) · [Thermal input](../firmware/src/ble_protocol/control/thermal_protection.c)

If temperature mode is selected and its input is unavailable, thermal protection clears an active overheat state and the fan task falls back to **Always on**. The selected mode stays in committed configuration. The regulator preserves its hysteresis state during fallback and resumes temperature control automatically with a valid input. Selecting temperature mode through Apply from another mode requires a ready input. Profile application skips an unavailable mode field and returns its reason. Changing the saved mode to Always on disables temperature-driven demand. The emergency overheat threshold still applies. [Thermal protection](../firmware/src/ble_protocol/control/thermal_protection.c) · [Mode and demand](../firmware/src/ble_protocol/control/fan_control.c) · [Apply validation](../firmware/src/ble_protocol/control/params.c)

A valid control temperature above 50 °C raises overheat in any mode. In temperature mode, reaching the configured maximum also raises overheat. With a valid reading, the flag clears once the temperature is at or below 50 °C and, in temperature mode, below the configured maximum. Overheat blocks new service operations and ends active ones. The LED signals overheat. The buzzer follows the overheat alarm setting, and a short button press can silence the current overheat without clearing it. Overcurrent protection remains active across modes and has a separate LED/buzzer alarm. [Thermal transitions](state-machines.md#overheat-protection) · [Operation guards](../firmware/src/ble_protocol/control/operation_manager.c) · [Buzzer](../firmware/src/ble_protocol/hw/overheat_alarm.c)

## Overcurrent protection

**Maximum output** is the global persisted ceiling. The fan task reads it from the committed RAM snapshot loaded from flash at startup. The protection ceiling starts at 100% internally and lives in RAM. The saved maximum and the temporary ceiling together cap all requests. Manual saves may raise or lower the configured maximum. A protection search is bounded by the preceding confirmed cap, the pre-adjustment applied output, and the saved maximum. During a search, extended BLE status reports the preceding confirmed ceiling and the active protection phase. Applied output is reported separately in metrics. After a confirmed correction, temperature mode also reports the temporary lower end as the smaller of base speed and the confirmed ceiling. Always on uses the corrected ceiling on its base speed request. [Limiter](../firmware/src/ble_protocol/control/output_limit.c) · [BLE status encoding](../firmware/src/ble_protocol/ble/fan_status_ble.c)

1. The INA226 monitor owns sensor I²C access and checks conversion readiness every 500 ms. It caches complete voltage/current pairs with a read timestamp, sequence, and a conservative lower bound for the start of the averaging window. Waiting retains the cached sample. I²C failure invalidates it. Hardware averaging remains at 512 samples. The hardware ALERT function is disabled.
2. A valid window taken entirely at the current applied output starts adjustment as soon as its measured current exceeds the threshold. The limiter waits for any admitted configuration write and uses the same reading for the formula when it is still fresh and describes the current output. An eligible sample must be at most 3.5 seconds old.
3. The formula requests `floor(applied percentage × threshold mA / measured mA)`, bounded by zero and the search ceiling. After the driver applies each probe, the limiter waits for the first eligible conversion whose entire averaging window follows that change. A reading at or below threshold allows the next increase of up to five percentage points or confirms the search ceiling.
4. A reading above threshold at the formula or upward probe causes one five-point rollback. A new conversion at the rollback confirms the limit if within threshold. If still above threshold, it causes 0% and a latched fault. Service probes enforce the configured minimum. Temperature and Always on probes can go below the base, including to 0%.
5. A valid reading above the threshold at already-applied 0% causes the distinct overload-without-load fault. A zero probe with a valid reading within threshold confirms a 0% ceiling.

A conversion that straddled a change cannot verify the new output even if it was read after the change. Each step advances as soon as an eligible measurement is available. At the configured averaging rate, a conversion takes about 2.69 seconds, and the conversion already in progress at the output change may need to be skipped. The limiter keeps monitoring in `LIMITED`. A fresh 119.2 mA measurement at a confirmed 95% limit starts another adjustment when the threshold is 100 mA.

[Current monitor](../firmware/src/metrics/ina226.c) · [Exact events and guards](state-machines.md#output-limit-and-reset)

For a non-service example, take 100% applied output, a 100 mA threshold, and a reading of 119.2 mA. The initial probe is `floor(100 × 100 / 119.2) = 83%`. If eligible measurements at 83% and 88% are within threshold, the limiter tries 93%. If 93% exceeds threshold, the limiter returns to 88% and checks a new measurement there. With a 90% base in temperature mode, a confirmed 88% limit makes the temporary running range 88–88%.

### Sensor loss, timeouts, and reset

- In ordinary operation, INA226 failure leaves output controlled by the selected mode and existing ceiling. The client warns that current monitoring is unavailable.
- With INA226 failure during adjustment, the limiter enters `SENSOR_ERROR` with a cap no greater than the configured base, current cap, or actual output. Once the driver applies the fallback, it enters `MINIMUM`. A valid conversion at the fallback within threshold restores the preceding state and cap. A conversion above threshold latches a fault.
- If an admitted configuration write is still running after 9 seconds in `WAIT_CONFIG` and no eligible reading has cleared the overload, the limiter enters `FAULT` directly. A missing eligible reading in `MEASURE` times out at 12 seconds. A probe without eligible measurement or a fallback without applied-output feedback times out at 9 seconds. These paths enter `VERIFY_TIMEOUT` with a conservative cap. A later reading above threshold can escalate it to `FAULT`. An accepted reset or reboot clears the timeout.
- **Reset overload** sends one command for both a latched overload and a temporary limit. The firmware requires a valid conversion at the applied output within threshold, checks again in the fan task, and reports the result by sequence. A reset leaves the saved maximum and profiles intact. Reboot also restores the saved ceiling. A new conversion above threshold after boot can trigger protection again.

[Limiter transitions](state-machines.md#output-limit-and-reset) · [Client reset command](../pcwcc/src/pcwcc/core_logic/control.py) · [Capturing the serial journal](firmware.md#runtime-and-diagnostics)

## Calibration and automatic setup

Both operations start with preflight. Calibration tests the configured control type. Setup tests DC and PWM, measures RPM, chooses the type and enabled channels, and delegates the base-speed search to calibration. All tests go through the shared limiter. Setup may commit base speed, control type, and monitoring settings for Fans 2–4. Standalone calibration commits base speed. [Preflight](../firmware/src/ble_protocol/control/operation_preflight.c) · [Operation code](../firmware/src/ble_protocol/control/operation_manager.c)

The tachometer mux spends 800 ms on each of four channels. Accepted pulse periods span 10–250 ms, corresponding to approximately 120–3,000 RPM for two pulses per revolution. The window and edge guards let slow tachometers contribute valid samples. Display RPM uses its own filter. Calibration sees raw observation windows with start and completion times, including empty windows. A baseline run is followed by stopped startup attempts. An attempt starts observing only after the driver applies the requested output and it settles. It needs three consecutive fresh windows per monitored channel and at least a 50 RPM increase over baseline on Fan 1. Loss of rotation before commit requires confirmation again. Interruption by protection requires new windows after the output is reapplied. [Tachometer sampling](../firmware/src/metrics/metrics.c) · [Calibration transitions](state-machines.md#calibration)

The operation's draft lives in RAM separate from committed settings. Final confirmation checks applied percent, control type, rotation, and limiter verification. Active adjustment postpones confirmation, and a confirmed ceiling below the target fails the operation. PCWCC attempts to cancel an ongoing service operation on close. The firmware independently handles owner disconnect. Either event before the commit boundary discards the draft and stops the operation. The commit at `COMMITTING` runs to its result and checks the captured factory reset revision. Operation status goes to the owning session. [Service lifecycle](state-machines.md#service-operation-lifecycle) · [Operation commit](../firmware/src/ble_protocol/control/operation_manager.c)

## Configuration and validation

| Scope | Lifetime and readers |
| --- | --- |
| Client edits and connection-owned pending packet | Unapplied edits. Firmware stages a field-mask draft for its connection. Discard, disconnect, or successful Apply clears it |
| Committed configuration | Versioned `params/config` NVS record with all parameters and the global maximum. A matching RAM snapshot is loaded at boot and consumed by the fan task |
| Operation draft and override | Operation-local settings candidate and requested hardware command. Ending before commit discards the candidate |
| Protection and effective mode | Temporary ceiling, measured status, thermal fallback, and displayed effective mode for the current run |
| Profiles | Independent slots `profiles/p1`–`p3` and metadata `profiles/m1`–`m3`. Clearing a profile leaves its metadata. Factory reset keeps both |

Apply merges the supplied fields into the current committed snapshot under the commit mutex. The firmware validates the full candidate, writes one versioned record, then publishes the new RAM snapshot after a successful save. It forces Fan 1 monitoring on. Fan 2–4 monitoring and the overheat buzzer flags are boolean. The control type must be DC or PWM and the selected mode must be supported. Temperatures and shutdown delta are integers in 0–150 °C. Maximum temperature must be greater than start temperature, and shutdown delta must be below start temperature. The committed base must be at least 10% and at or below the saved maximum, itself 10–100%. [Firmware validator](../firmware/src/ble_protocol/control/params.c) · [Settings and formats](settings.md)

A regular Apply or profile with a valid base speed of 80% and saved maximum of 70% commits a base of 70%. A lower saved maximum also reduces an incompatible base in the same write. A base above 100% fails normal validation. Profiles may skip invalid fields, unavailable temperature-mode changes, or conflicting temperature fields. Applying a profile returns an applied-field mask and reasons. A field can be both applied and marked `OUTPUT_MAX` to indicate it was capped. The original profile remains available for later applications. A successful BLE write for a known profile confirms command handling. The application result and per-field reasons come from status. [Parameter and profile validation](../firmware/src/ble_protocol/control/params.c) · [GATT handlers](../firmware/src/ble_protocol/ble/gatt.c)

Apply, Apply profile, and Save maximum are rejected during active protection adjustment with `OUTPUT_ADJUSTING`. Regular Apply retains the pending draft on rejection until retry, discard, or disconnect. An already accepted write can finish and the limiter takes one configuration snapshot after it. Settings can be saved during a latched fault. Changing settings through Apply does not itself clear that fault. An accepted transition to Inactive and a service cancellation can issue stop requests through the fan task. [Configuration lock](../firmware/src/ble_protocol/control/output_limit.c) · [Apply and write gate](../firmware/src/ble_protocol/control/params.c)

Factory defaults include base 50% and maximum 100%. Legacy installations can load `ver`, `blob`, and `output_max`. A `config_started` marker on the first new-format save prevents a missing new record from silently reactivating legacy settings. A load failure blocks output and sets `CONFIG_STORAGE_ERROR`. Saving a valid maximum or completing factory reset can restore the record. Factory reset uses a separate durable transaction marker and clears configuration and BLE trust while retaining profiles. [NVS load and commit](../firmware/src/ble_protocol/control/params.c) · [Factory reset transaction](state-machines.md#configuration-and-reset-transactions)

## BLE and the desktop client

The pairing window and exchange own enrollment. The operational connection authenticates against stored trust. The GATT transport binds protected subscriptions to the authorized connection and routes events from their owners. See the [pairing and access transitions](state-machines.md#ble-access) for button timing, connection guards, and the exchange.

| Data | Delivery |
| --- | --- |
| Temperature, RPM, voltage, current, and applied output | Initial reads and telemetry notifications to the authorized subscriber |
| Device diagnostics and fan status | Initial reads and changed-snapshot notifications |
| Operation progress and terminal result | Status read on connection and progress/result notifications to the owning subscriber |
| Settings and profiles | Draft and action writes, parameter status/result, and explicit readback where the action needs it |
| Effective mode and current limit | Subscribe and take the initial snapshot, then process changed-status notifications. A racing initial read cannot replace a newer notification during the same read. The limit notification carries overload reset sequence and result |

`CONFIG_OUTPUT_RANGE` (`…e1c`) is the preferred read/notify status characteristic. Its eight-byte v3 status adds temporary minimum and overload-without-load to the base fields and reports the confirmed ceiling during adjustment. If absent, the client can use the v2 limit status and commands on `…e19`. Fan mode v1 uses `…e1a` and reports selected mode, effective mode, and temperature input state. Older read-only statuses can supply an initial snapshot. Reset confirmation requires notifications. The GUI has no periodic GATT read loop for these statuses. [Status encoding](../firmware/src/ble_protocol/ble/fan_status_ble.c) · [Client subscription and bootstrap](../pcwcc/src/pcwcc/gui_logic/worker_flows.py) · [Client protocol commands](../pcwcc/src/pcwcc/core_logic/control.py)
