# 06 — Acceptance tests / trace contracts

Write pure host-runnable tests for each FSM and orchestrator sequence; hardware mock adapters record ordered commands and published events. Run current project tests too, including existing pairing/GATT/security/regression tests.

## Load
- IDLE+button -> OUTPUT_ON; only ACK begins settle; no voltage sample before 1000 ms.
- STARTUP+valid voltage -> exactly one normal or combined buzzer cue; WORKING after completion; no double activation on duplicate button.
- WORKING+valid sustained current below threshold -> STOPPING at configured timeout; OUTPUT_ON maintained through voltage request and completion; OUTPUT_OFF afterward.
- WORKING+invalid current resets pending low-current timer, no accidental OFF.
- Persistent WORKING crosses Deep Sleep while held output stays active.
- Shutdown INA failure -> no error tone and eventual GPIO OFF.

## Buzzer
- STARTUP INA failure -> **exactly three** pulses once per operation, with gaps, then DONE.
- PERIODIC INA failure × 100 -> zero pulses.
- SHUTDOWN INA failure -> zero error pulses and DONE.
- Repeated duplicate or stale failed request does not repeat pattern.
- Success voltage below threshold -> configured combined startup cue; above threshold -> configured normal cue.
- buzzer_enabled=false still completes startup/shutdown immediately/safely.
- Timing/cue run does not block dispatcher; no sleep before cue completion.
- Preserve existing delayed low-voltage warning behavior for valid periodic readings; invalid readings are not asserted to be low voltage.

## BLE
- No button after boot -> **zero** NimBLE init, zero advertising, sleep.
- First button -> one lazy init; repeated button while STARTING -> no duplicate init.
- ADVERTISING button -> deadline renewed; old timeout ignored.
- ADVERTISING expires without clients -> BLE stop fully confirmed, OFF, sleep allowed.
- ADVERTISING connects -> CONNECTED; CLIENT_DISCONNECTED -> STOPPING always; no advertising restart despite remaining time.
- CONNECTED window timeout -> stay connected; eventual disconnect -> STOPPING.
- STOPPING press -> pending restart; after BLE_STOPPED -> STARTING exactly once.
- BLE disconnect submission failure -> stop the session instead of waiting for a callback that was never requested.
- Late GAP callbacks cannot mutate a new session.
- GPIO19 pairing/reset behavior preserved.

## Integration/power
- Load ON while BLE advertising; 60 s elapse -> radio OFF, load still ON and sleep permitted.
- Initial GPIO ON -> 1000 ms delay -> *fresh* INA read -> beep (not pre-load voltage).
- Request shutdown -> voltage read and optional cue -> GPIO OFF, never opposite order.
- Two simultaneous callbacks don't break state or generate duplicate beep.
- Timeout/failure paths have finite cleanup; no infinite wait or stale work preventing sleep.
- ISR publication during sleep preparation is delivered and aborts sleep; queue overflow is staged or raises a sticky sleep-blocking delivery fault.
- Action-queue rejection for LOAD_ON, LOAD_OFF, BLE start/stop, buzzer playback, settle timer, and INA request is delivered to the owning FSM; LOAD_OFF rejection never reports IDLE while output is confirmed/possibly ON.
- Rejected buzzer cancellation during shutdown waits for the active warning's correlated completion before GPIO OFF.
- Timer-start failures during startup and shutdown do not leave WAIT_SETTLE/MEASURING active; startup emits one error pattern, shutdown reaches the GPIO-OFF attempt.
- INA226 host fault injection verifies conversion timeout, I2C timeout, and the 10.45 s maximum expected call path under timeout assumptions.
- Build succeeds with installed ESP-IDF version; no silent changes in BLE security/storage/GATT contract.

## Deliverable evidence
For each test provide its name and pass/fail result. Provide one event/command trace for cold boot, startup success, startup INA error, shutdown INA error, BLE disconnect and BLE window expiry. State explicitly if physical low-power current was **not** measured, and do not claim an energy improvement without measurements.
