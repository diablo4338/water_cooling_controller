# State machines and event routing

[Runtime behavior](runtime.md) describes the user-visible policies. [Settings](settings.md) lists the knobs and timing constants. [Design milestones](design-milestones.md) records how the current policies were chosen.

## Owners and delivery

| Owner | Instances | Entry point |
| --- | --- | --- |
| BLE access | One | `fsm_dispatch()` |
| Pairing window | One | `pair_mode_dispatch()` |
| Pairing exchange | One | `pair_state_dispatch()` |
| Temperature demand | One | `temp_demand_dispatch()` |
| Overheat | One | `thermal_dispatch()` |
| Output limit | One, across modes and service operations | `output_limit_dispatch()` |
| Operation lifecycle | One | `operation_dispatch_locked()` |
| Operation preflight | One, shared by both operations | `preflight_dispatch()` |
| Fan calibration | One, used on its own or by automatic setup | `op_calib_dispatch()` |
| Automatic setup | One | `op_setup_dispatch()` |
| PCWCC connection and command lifecycle | One of each per app model | `AppModel.handle_connection_event()`, `AppModel.dispatch()` and result handlers |

The firmware also tracks state in the [measurements, tachometers, and indicators](#measurements-tachometers-and-indicators) and the [configuration transaction](#configuration-and-reset-transactions). Their input conditions and side effects are listed below. Internal events can be synchronous dispatch calls, NimBLE queue entries, or work observed on the fan task's next iteration. A BLE notification carries a snapshot or command result through the transport layer. It does not advance the firmware state machine that produced it.

## BLE access

Initial state `UNPAIRED_IDLE`. The states below omit the `FSM_STATE_` prefix, and events omit `FSM_EVT_`.

| From | Incoming event and producer | To and effect |
| --- | --- | --- |
| Any, with an available connection slot | `CONNECT` from GAP | Same state. Binds the sole active connection handle |
| `UNPAIRED_IDLE` | `TRUST_LOADED` from trust storage | `PAIRED_UNAUTH`. Startup loading and successful enrollment both use this event |
| `PAIRED_UNAUTH` | `AUTH_OK` from MAIN GATT after proof verification on the current connection | `AUTHED`. Clears the challenge and grants protected GATT access |
| `PAIRED_UNAUTH` | `AUTH_FAILED` from MAIN GATT on the current connection | Same state |
| Any with matching active handle | `DISCONNECT` from GAP | Releases connection and session bindings. `AUTHED` returns to `PAIRED_UNAUTH` |
| Any | `TRUST_RESET` from trust storage | `UNPAIRED_IDLE`. Clears session guards |

GATT access checks, notification routing, and GAP consume the active and authorized handles. GAP also forwards link closure to pairing mode and cleans up connection-scoped configuration and operations. [Access owner](../firmware/src/ble_protocol/pairing/state.c) · [GAP](../firmware/src/ble_protocol/ble/gap.c)

## Pairing window

Initial state `CLOSED`. Events below omit `PAIR_WINDOW_`. The NimBLE host task serializes window transitions.

| From | Incoming event and producer | To and outgoing effect |
| --- | --- | --- |
| Closed or open | `OPEN_REQUEST` queued by the physical button task after a hold longer than 3 s | `OPEN` with a 60 s deadline when key preparation succeeds. Resets and starts the exchange. Updates GAP advertising or closes the existing link to allow pairing |
| `OPEN` | `LINK_CLOSED` from GAP | Keeps the original deadline and prepares a fresh exchange. Expiry or preparation failure closes the window |
| `OPEN` | `EXPIRED` queued by the timer | `CLOSED` once the actual deadline has passed. Clears the session and restores normal advertising or closes the unauthenticated link |
| `OPEN` | `PAIR_COMPLETED` from PAIR GATT | `CLOSED`. Clears the session after trust was saved |
| Any | `FACTORY_RESET` from reset handler | `CLOSED`. Clears the session |

The window sends `PAIR_EVT_SESSION_RESET` to the exchange when clearing a session and `PAIR_EVT_SESSION_STARTED` when a new session is ready. The indicator reads whether the window is open. [Window owner](../firmware/src/ble_protocol/pairing/pair_mode.c) · [Button and reset events](../firmware/src/ble_protocol/app/app_tasks.c)

## Pairing exchange

Initial state `IDLE`. Events below omit `PAIR_EVT_`. The PAIR GATT handlers enforce the connection and verify each protocol message before advancing.

| From | Incoming event and producer | To and outgoing effect |
| --- | --- | --- |
| `IDLE` | `SESSION_STARTED` from pairing window | `WAIT_HOST_PUB` |
| `WAIT_HOST_PUB` | `HOST_PUB_ACCEPTED` from PAIR GATT | `WAIT_CONFIRM`. The client can read the device key and proof for the exchange |
| `WAIT_CONFIRM` | `CONFIRM_ACCEPTED` from PAIR GATT | `WAIT_FINISH` |
| `WAIT_FINISH` | `FINISH_ACCEPTED` from PAIR GATT after persistent trust registration | `FINISHED`. GATT sends `PAIR_COMPLETED` to the window and schedules link closure. Trust registration updates BLE access |
| Any | `SESSION_RESET` from pairing window | `IDLE`. Clears the exchange connection guard |

The host stores the resulting trust material and uses MAIN authorization on operational connections. [Exchange table](../firmware/src/ble_protocol/pairing/pair_state.c) · [Pair and authorize handlers](../firmware/src/ble_protocol/ble/gatt.c) · [Trust storage](../firmware/src/ble_protocol/pairing/storage.c)

## Temperature demand

Initial state `TEMP_DEMAND_IDLE`. The fan regulator generates the events and consumes the resulting state.

| From | Incoming event and guard | To and output |
| --- | --- | --- |
| Either | `TEMP_DEMAND_CONTROL_DISABLED` when the regulator sees a selected mode other than temperature control | `TEMP_DEMAND_IDLE` |
| Either | `TEMP_DEMAND_START_REACHED` with temperature at or above the start threshold | `TEMP_DEMAND_RUNNING`. Requests the temperature curve |
| Either | `TEMP_DEMAND_STOP_REACHED` at or below start minus shutdown delta | `TEMP_DEMAND_IDLE`. Requests 0% |
| Either | Between thresholds or temporary fallback while temperature mode remains selected | Preserves state. During fallback the regulator requests the base fan speed |

The regulator sends the request to the output limiter. The fan task publishes the selected mode, effective mode, and thermal input status to BLE. Configuration owns the selected mode. [Fan task and demand](../firmware/src/ble_protocol/control/fan_control.c)

## Overheat protection

Initial state `THERMAL_DISABLED`. Each fan-loop observation compares the selected mode and the current ADC/NTC reading before dispatching.

| From | Incoming event and guard | To and outgoing effect |
| --- | --- | --- |
| Any | `THERMAL_CONTROL_DISABLED` when the selected mode is Always on or Inactive | `THERMAL_DISABLED`. Clears overheat |
| Any, with temperature control selected | `THERMAL_SENSOR_UNAVAILABLE` on invalid thermal input | `THERMAL_TEMPERATURE_UNAVAILABLE`. Clears overheat and reports the input reason |
| Any, with valid control temperature | `THERMAL_TRIP` when temperature reaches the configured maximum | `THERMAL_OVERHEAT`. Records the threshold on entry |
| Any, with valid control temperature | `THERMAL_COOL` when below the current threshold and, while overheating, also below the recorded threshold | `THERMAL_NORMAL`. Clears overheat |

The thermal owner returns the input observation to the fan task and writes its flags to `device_status`. The operation manager uses the overheat flag to reject or end service. LED and buzzer tasks observe the same flag. The button silence request goes to the buzzer and preserves thermal state. [Thermal owner](../firmware/src/ble_protocol/control/thermal_protection.c) · [Operation manager](../firmware/src/ble_protocol/control/operation_manager.c)

## Output limit and reset

Initial state `OUTPUT_LIMIT_NORMAL` with an internal cap of 100%. Transition rows omit the `OUTPUT_LIMIT_` state prefix and `LIMIT_EVENT_` event prefix. The fan task generates all dispatch events from the INA226 cache, the configuration gate, applied output, and pending stop or reset requests. A reading qualifies when it is valid, finite, at most 3.5 seconds old, and its entire averaging window is at the applied output. Probe verification also requires the window to begin at or after entry into the step. The firmware compares current strictly above the threshold for overload.

| From | Incoming event and guard | To and outgoing effect |
| --- | --- | --- |
| `NORMAL`, `LIMITED` | `CURRENT_EXCEEDED` on a fresh INA226 window at nonzero applied output over threshold and enabled saved maximum | `WAIT_CONFIG`. Records previous state/cap and the applied output. Takes ownership of output and closes admission for new writes |
| `WAIT_CONFIG` | `CONFIG_READY` when no admitted write remains | `MEASURE`. Captures base and maximum from committed RAM |
| `WAIT_CONFIG`, `MEASURE` | `MEASUREMENT_CANCELLED` when a fresh window at the applied output is within threshold before calculation | Restores the previous state and cap |
| `MEASURE` | `MEASURED` with fresh current above threshold at the applied output | `VERIFY_FORMULA` with the bounded formula result. Service probe below its configured minimum goes to `FAULT` |
| `VERIFY_FORMULA`, `SEARCH_UP` | `ADVANCE_PROBE` on a complete post-application window within threshold at a nonzero step with room to increase | `SEARCH_UP`. Requests up to five percentage points more |
| `VERIFY_FORMULA`, `SEARCH_UP`, `VERIFY_STEP` | `SAFE` on a complete post-application window within threshold when there is no further upward step | `LIMITED`. Confirms the ceiling, including 0% |
| `VERIFY_FORMULA`, `SEARCH_UP` | `OVER_LIMIT` on a complete post-application window above threshold at a nonzero step | `VERIFY_STEP`. Requests five points less, with the same service minimum guard |
| `VERIFY_STEP`, `SENSOR_ERROR`, `MINIMUM`, `VERIFY_TIMEOUT` | `OVER_LIMIT` on a fresh window above threshold at the applied nonzero output | `FAULT`. Commands 0% |
| `MEASURE` or probe states | `SENSOR_UNAVAILABLE` when INA226 is offline | `SENSOR_ERROR`. Caps output at or below base and actual output |
| `SENSOR_ERROR` | `MINIMUM_APPLIED` when the driver has applied the conservative cap | `MINIMUM`. Releases the adjustment lock and retains the fallback cap while waiting for a new conversion |
| `MINIMUM` | `SENSOR_RECOVERED` with a fresh window at the applied output within threshold | Restores the previous state and cap |
| `WAIT_CONFIG` | `VERIFICATION_TIMED_OUT` after 9 s with an admitted write still running and no eligible reading within threshold | `FAULT` at 0% |
| `MEASURE`, probe states, `SENSOR_ERROR` | `VERIFICATION_TIMED_OUT` after 12 s without an eligible measurement in `MEASURE`, or after 9 s without a verified probe or applied fallback | `VERIFY_TIMEOUT` at a conservative cap |
| Any outside `FAULT` | `CURRENT_WITHOUT_LOAD` on a fresh window above threshold at already applied 0% | `FAULT`. Reports the overload-without-load flag |
| Adjustment active | `STOP_REQUESTED` from service stop or transition to Inactive | Aborts the search, restores previous state/cap, and issues 0% for the stop iteration |
| Settled states admitting reset | `RESET` after the fan task rechecks fresh current within threshold at the applied output | `NORMAL` at internal cap 100%. Clears the overload fault and temporary cap together. Reset in `NORMAL` completes without a state change |

An active adjustment holds the protection's percentage and most recently applied DC/PWM type. The limiter returns the next command to the driver and receives applied feedback through `output_limit_output_applied()`. It reports its status to operation/preflight, sends flags to `device_status`, and exposes a snapshot to the BLE publisher. `VERIFY_MINIMUM`, `CONFIRMING`, and `START_BLOCKED` are retained wire IDs. `WAIT_CONFIG` and `SEARCH_UP` are live internal states encoded as `MEASURE` and `VERIFY_STEP` respectively. The existing overload-without-load status bit and v3 flag carry the new current-based diagnosis.

The separate reset result progresses from `OUTPUT_LIMIT_RESET_NONE` at boot to `PENDING` when GATT admits the request and increments its sequence, then `APPLIED` or `REJECTED` when the fan task rechecks it. Eligible states are `NORMAL`, `LIMITED`, `MINIMUM`, `SENSOR_ERROR`, `VERIFY_TIMEOUT`, and `FAULT`, with a fresh current sample at the applied output within threshold. A busy or unhealthy submission can fail at GATT admission. The client waits for the matching terminal sequence. [Limiter owner and configuration gate](../firmware/src/ble_protocol/control/output_limit.c) · [State IDs](../firmware/src/ble_protocol/control/output_limit.h) · [Client reset](../pcwcc/src/pcwcc/core_logic/control.py)

## Service operation lifecycle

Initial state `OP_PHASE_READY`. Events below omit `OP_EVENT_`, and phases omit `OP_PHASE_`. Dispatch is serialized with the state lock. The fan task advances the operation and owns its output/commit effects.

| From | Incoming event and producer | To and outgoing effect |
| --- | --- | --- |
| `READY` | `START` from an authorized GATT command after admission checks | `QUEUED`. Binds owner, operation type, and reset revision |
| `QUEUED` | `BEGIN_PREFLIGHT` from fan task | `PREFLIGHT`. Starts mode-specific electrical tests |
| `PREFLIGHT` | `PREFLIGHT_PASSED` from successful preflight | `RUNNING`. Starts calibration or automatic setup |
| `RUNNING` | `STEPS_SUCCEEDED` from the operation step | `WAITING_FOR_OUTPUT_CONFIRMATION` |
| `WAITING_FOR_OUTPUT_CONFIRMATION` | `OUTPUT_LIMIT_ADJUSTING` from limiter snapshot | `WAITING_FOR_OUTPUT_LIMIT` |
| `WAITING_FOR_OUTPUT_LIMIT` | `OUTPUT_LIMIT_RESOLVED` once adjustment releases output | `WAITING_FOR_OUTPUT_CONFIRMATION`. Restarts confirmation |
| `WAITING_FOR_OUTPUT_CONFIRMATION` | `ROTATION_LOST` from calibration observation | `RUNNING`. Calibration is told to acquire confirmation again |
| `WAITING_FOR_OUTPUT_CONFIRMATION` | `OUTPUT_CONFIRMED` from applied-output, limiter, and calibration checks | `COMMITTING`. Sends only the operation's permitted changes and captured revision to configuration |
| Active before commit | `CANCEL` from the owner or `OWNER_DISCONNECTED` from GAP | `CANCELLING`. A disconnect releases the owner. Fan task stops service output and discards the operation draft |
| Active, past the queue | `FINISHED` from fan task on completion or failure | `READY`. Sends terminal result to the operation's client while it remains connected |

The wire/UI status is `OP_STATE_IDLE`, `OP_STATE_IN_SERVICE`, `OP_STATE_DONE`, or `OP_STATE_ERROR`. Progress text is sent while in service. Overheat, protection failure, driver/confirmation failure, and a ceiling below the service target can end the operation with an error. `COMMITTING` is the cutoff for cancellation. [Lifecycle and guards](../firmware/src/ble_protocol/control/operation_manager.c) · [Wire states](../firmware/src/ble_protocol/control/operation_manager.h)

### Preflight

Initial phase `PREFLIGHT_PHASE_IDLE`. `PREFLIGHT_EVENT_TEST_STARTED` enters `PREFLIGHT_PHASE_TEST` and requests 100% through the fan override and shared limiter. Calibration tests the selected control type. Setup tests DC and then PWM. `PREFLIGHT_EVENT_TEST_PASSED` records the verified cap and advances to the next test or returns to `IDLE` after the final one. `PREFLIGHT_EVENT_RESET` returns to `IDLE` on teardown. The parent receives caps or an error, and progress goes to the operation owner over BLE. [Preflight](../firmware/src/ble_protocol/control/operation_preflight.c)

### Calibration

Initial phase `CALIB_PHASE_IDLE`. The table omits the `CALIB_PHASE_` and `CALIB_EVENT_` prefixes. Output inputs come from fan driver feedback. Rotation inputs come from raw tachometer windows.

| From | Incoming event | To and outgoing effect |
| --- | --- | --- |
| `IDLE` | `STARTED` from calibration start | `WAIT_STOP_OUTPUT`. Requests 0% |
| `WAIT_STOP_OUTPUT` | `STOP_APPLIED` | `STOP_WAIT`. Starts the stop timer |
| `STOP_WAIT` | `STEP_REQUESTED` when the stop timer completes | `WAIT_OUTPUT`. Requests baseline or startup probe |
| `WAIT_OUTPUT` | `BASELINE_APPLIED` | `WAIT_BASELINE`. Starts observing after settling |
| `WAIT_BASELINE` | `BASELINE_OBSERVED` | `WAIT_STOP_OUTPUT`. Captures the baseline and requests a new stop |
| `WAIT_OUTPUT` | `STEP_APPLIED` | `CONFIRM_ROTATION`. Starts a fresh observation interval |
| `STOP_WAIT`, `WAIT_BASELINE`, `CONFIRM_ROTATION`, `ROTATION_CONFIRMED` | `OUTPUT_INTERRUPTED` | Restarts at `WAIT_STOP_OUTPUT` or `WAIT_OUTPUT` as appropriate. Fresh windows are required after applied feedback |
| `CONFIRM_ROTATION` | `ROTATION_CONFIRMED` | `ROTATION_CONFIRMED`. Provides the candidate to the parent operation |
| `ROTATION_CONFIRMED` | `ROTATION_LOST` before commit | `CONFIRM_ROTATION` |
| `CONFIRM_ROTATION` | `ROTATION_TIMED_OUT` with a higher candidate available | `WAIT_STOP_OUTPUT`. Starts another stopped attempt at five points more. Exhausting the range returns an error |
| Any | `FINISHED` from teardown | `IDLE` |

Calibration updates the operation-local draft and reports progress to its parent. The parent performs the persistent commit only after final output and rotation confirmation. [Calibration transitions and RPM guards](../firmware/src/ble_protocol/control/operation_manager.c)

### Automatic setup

Initial phase `SETUP_PHASE_IDLE`. `SETUP_EVENT_DC_OUTPUT_SET` enters `SETUP_PHASE_WAIT_DC` after requesting DC from the fan override. After the DC observation, `SETUP_EVENT_PWM_OUTPUT_SET` enters `SETUP_PHASE_WAIT_PWM` after requesting PWM. The setup compares RPM to select control type and monitored channels, updating the operation-local draft. `SETUP_EVENT_CALIBRATION_STARTED` enters `SETUP_PHASE_CALIBRATION` and delegates the base-speed search to calibration. `SETUP_EVENT_FINISHED` returns to `SETUP_PHASE_IDLE`. The parent receives success or error and owns the final commit. [Setup transitions](../firmware/src/ble_protocol/control/operation_manager.c)

## Measurements, tachometers, and indicators

| Owner and states | Input to transition | Outgoing data and recipients |
| --- | --- | --- |
| ADS1115 health in `metrics_record_adc_batch()`. `METRICS_ADC_UNKNOWN`, `AVAILABLE`, `OFFLINE`, `RECOVERING` | Failed I²C batch moves to `OFFLINE` and resets the success count. The first good batch after failure moves to `RECOVERING`. Three consecutive successes give `AVAILABLE`, both at startup and after failure | The thermal owner and BLE telemetry read the state and channel snapshots. An empty thermistor value is a channel observation |
| INA226 monitor. `INA226_WAITING`, `READY`, `OFFLINE` | 500 ms check. A conversion still in progress or one completing during the register reads yields `WAITING`, a coherent completed conversion yields `READY`, and I²C error yields `OFFLINE`. A complete successful read after failure restores sample validity | Read timestamp, averaging-window start bound, sequence, current, voltage, validity, and state go through one cache to the limiter and BLE telemetry. Sensor failure goes to device diagnostics |
| Tachometer diagnostics in `fan_channel_update()`, four instances. `FAN_STATE_IDLE`, `STARTING`, `RUNNING`, `STALL`, `IN_SERVICE` | Active service selects `IN_SERVICE`. Output off selects `IDLE`. With monitored output on, idle/service enters `STARTING`, positive RPM advances to `RUNNING`, and startup or RPM-loss deadline yields `STALL`. Positive RPM leaves `STALL`. An unmonitored running channel follows RPM presence without stall checks | Fan status snapshots go to BLE. Stall entry is logged on the serial console. A separate raw tachometer window feeds calibration |
| Aggregate diagnostics. `DEVICE_STATE_OK`, `DEVICE_STATE_ERROR` | Owners update their bits in the error mask. Zero yields `OK`, and any set bit yields `ERROR` | Updated status goes to authorized BLE subscribers. The operation manager, LED, and buzzer read relevant flags |
| Status LED. `STATUS_LED_MODE_OFF`, `ON`, `BLINK_SLOW`, `BLINK_FAST`, with a factory reset indication | Factory reset result runs a finite blink sequence. Steady priority is output alarm, pairing, overheat, generic error, then off | GPIO output |
| Buzzer. Off, pulse, pause, and overheat silenced | Output alarm or enabled overheat requests sound. Short button press silences the current overheat alarm. Leaving overheat clears that silence. Output alarm continues to request sound independently | GPIO output |

[Sensor and tachometer sampling](../firmware/src/metrics/metrics.c) · [INA226 cache](../firmware/src/metrics/ina226.c) · [Fan diagnostics](../firmware/src/ble_protocol/control/fan_control.c) · [Device status](../firmware/src/ble_protocol/control/device_status.c) · [LED](../firmware/src/ble_protocol/hw/status_led.c) · [Buzzer](../firmware/src/ble_protocol/hw/overheat_alarm.c)

## Configuration and reset transactions

| Workflow | Input and progression | Output and recipients |
| --- | --- | --- |
| Regular settings | Parameter packet stages a field-mask draft for the active connection. Apply merges it into committed RAM, validates the candidate, acquires write admission, saves the complete record, then publishes RAM. Discard or disconnect drops the draft | Parameter status goes to the requesting client. The fan task consumes the published snapshot |
| Profile application | Slot number selects a saved packet. Firmware selects valid fields, clamps base speed, and uses the shared commit. Profile create, clear, and metadata edits are separate slot operations | Per-field application status goes to the requester. The fan task consumes committed settings |
| Service result | Parent moves into `COMMITTING` and submits operation-local masked changes with its reset revision | Shared commit publishes the new RAM snapshot and returns the result to the parent |
| Factory reset | Physical hold longer than 10 s queues work in NimBLE. The persistent marker records an incomplete reset until trust and configuration are both written. Boot resumes any marked reset before runtime starts | Sends pairing-window reset and trust reset, invalidates stale operation results via configuration revision, reports LED success/failure, and updates the link/advertising. Profile slots and metadata are retained |

[Parameter transaction](../firmware/src/ble_protocol/control/params.c) · [Profiles](../firmware/src/ble_protocol/control/profiles.c) · [Factory reset](../firmware/src/ble_protocol/app/app_tasks.c) · [Boot recovery](../firmware/src/ble_protocol/app/main.c)

## PCWCC connection and command lifecycle

| From | Incoming event and producer | To and effect |
| --- | --- | --- |
| `DISCONNECTED` | Manual `Action.CONNECT` or auto worker `CONNECT_REQUEST` for the current `auto_run_id` | `CONNECTING`. Assigns a `connection_id` |
| `CONNECTING` | Worker `OLD_SESSION_CLOSED` for the same connection request | Stays `CONNECTING` |
| `CONNECTING` | Worker `CONNECT_SUCCEEDED` with device | `CONNECTED`. Finishes the connect action |
| `CONNECTING` | Worker `CONNECT_FAILED` | `DISCONNECTED`. Clears session status |
| `CONNECTED` | `Action.DISCONNECT` | `DISCONNECTING`. Worker tears down notifications and link |
| `CONNECTED`, `DISCONNECTING` | Worker `LINK_CLOSED` | `DISCONNECTED`. Clears session state and preserves an active auto-connect run |
| Active auto-connect run | Stop request followed by `AUTO_STOPPED` from the matching run | `DISCONNECTED`. Clears run, command, and session state |

`AppModel.dispatch()` admits actions using the current UI state and returns a command to the worker. Asynchronous actions store an active action, command identity, and optional profile slot. Apply and profile-apply consume the matching parameter result. Successful Apply with a profile slot advances directly to `SAVE_PROFILE`, which ends on the matching profile write result. Calibration/setup ends on start failure or matching firmware `DONE`/`ERROR`. Maximum save and overload reset use command results. Runtime mode, limiter, and operation updates must match the active `connection_id`. The model supplies state to UI rendering, while worker signals deliver BLE results to the GUI. [Client model](../pcwcc/src/pcwcc/presentation.py) · [Worker flows](../pcwcc/src/pcwcc/gui_logic/worker_flows.py)
