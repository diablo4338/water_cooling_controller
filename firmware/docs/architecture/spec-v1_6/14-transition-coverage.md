# Formal transition coverage — v1.4 (normative)

This document closes event ordering and omitted-event ambiguity; it **does not create new functionality**. The source repository remains AS-IS, and this package is the target behavior.

## Execution model

1. Each cycle takes a monotonic `now` snapshot, consumes a bounded number of available messages, evaluates deadlines for **all three FSMs regardless of queue state**, executes accepted actions, then evaluates Deep Sleep eligibility.
2. Handlers never block waiting for events. A local deadline is authoritative even if a timer notification, callback, or completion message disappears. Physical I2C and GPIO calls are handled with their existing bounded API behavior.
3. A state transition or hardware operation requiring asynchronous completion has a current `operation_id`; wrong, expired, or duplicate IDs are ignored. The terminal action happens once. If completion and timeout are both observed on the same iteration, process at most one result, with precedence defined below.
4. Unlisted events in a state are **no-op** (optionally log at debug level) and may not reset deadlines. Do not turn ignored events into system-level errors or sleep vetoes.
5. An unsuccessful event publication is not a physical operation failure. Do not falsely report GPIO success, but do not create a global `delivery_fault` state.

## Priority at simultaneous boundaries

| Coincidence | Deterministic rule |
|---|---|
| Valid operation completion and local deadline on same iteration | Completion may win if it is known to match the live operation and was actually performed; execute terminal action once. If no valid completion is available, deadline wins. |
| BLE `CLIENT_DISCONNECTED` and 600-s connection deadline | Both converge on `READY_TO_SLEEP`; only one sleep request. |
| BLE `CLIENT_CONNECTED` vs 60-s advertising deadline | Serialize by event receipt order with a fresh state/deadline check. If advertising expiry was already committed, late connection cannot resurrect the BLE session; if connection was accepted first, apply the 600-s deadline. Do not invent a second advertising window. |
| BLE `BLE_READY` vs startup failure/timeout | A ready result is accepted only while STARTING and before the timeout decision is finalized. Otherwise ignore as stale. |
| Buzzer DONE vs playback deadline | Stop buzzer and complete operation once; no second beep. |
| Load shutdown signal completion vs late periodic measurement | Complete shutdown; late periodic result cannot bring load back to WORKING. |
| Thermal button vs ordinary periodic sample | Both are handled once; one thermal press fans out to Load and BLE. Button must not be reconstructed from INA periodic result. |
| BLE session finished vs load/buzzer mandatory operation ongoing | BLE remains `READY_TO_SLEEP`; power waits only for bounded physical operation or its local deadline. |

## Load FSM transition completeness

| State | Relevant event or condition | State/action | Deadline/no-event behavior |
|---|---|---|---|
| IDLE | THERMAL_BUTTON_PRESSED | STARTING; request real LOAD_ON | Handle bounded GPIO action failure; do not claim success |
| IDLE | Periodic sample | IDLE, no load-off reactivation | Next normal Deep Sleep |
| STARTING | Matching LOAD_ON applied | STARTING; start buzzer STARTUP settle | If buzzer completion missing, startup deadline → WORKING |
| STARTING | Matching LOAD_ON failure | ERROR; log real physical fault | No fake ON; handle physical fault separately |
| STARTING | STARTUP buzzer complete / bounded deadline | WORKING; keep load ON | Late completion ignored |
| WORKING | THERMAL_BUTTON_PRESSED | WORKING; no GPIO change | BLE handles press separately |
| WORKING | Valid low-current sample | WORKING; maintain/init low-current timer | Timeout becomes STOPPING |
| WORKING | Normal-current sample | WORKING; clear low-current timer | — |
| WORKING | INA failure (PERIODIC) | WORKING; reset low-current timer | No beep and no sleep veto |
| WORKING | Low-current deadline | STOPPING; request shutdown battery check with load ON | Shutdown feedback deadline bounds delay |
| STOPPING | Feedback complete / local deadline | STOPPING; force buzzer OFF before real LOAD_OFF | If confirmation missing, deadline still initiates GPIO OFF |
| STOPPING | Matching actual LOAD_OFF applied | IDLE; retain OFF | Ignore duplicates |
| STOPPING | Actual GPIO OFF failure | Bounded existing retry then ERROR | Never claim OFF based solely on timeout |
| ERROR | Periodic/irrelevant notifications | ERROR; no false recovery | Hardware-fault policy applies, not event-queue policy |

**Recovery**: valid RTC load state restores requested output; invalid RTC defaults to OFF. No startup beep is generated merely by restoring WORKING. STARTING and STOPPING are transient operations; recovery must not pretend a partial operation completed.

## BLE FSM transition completeness

| State | Trigger | Result |
|---|---|---|
| OFF | Qualified thermal activation | STARTING; initialize NimBLE once this wake; advertising deadline 60 s |
| OFF | No activation | Remain OFF; no NimBLE init, normal sleep after sampling |
| STARTING | Repeat press | Remain STARTING; update window deadline, never reinitialize |
| STARTING | Valid BLE_READY before deadline | ADVERTISING; advertise |
| STARTING | Init failure or finite startup timeout / expired window | READY_TO_SLEEP; do not retry init this wake |
| ADVERTISING | Repeat press | Remain ADVERTISING; update 60-s deadline |
| ADVERTISING | Accepted client connection | CONNECTED; deadline = connection time + 600 s |
| ADVERTISING | Window expired with no accepted connection | READY_TO_SLEEP; no stop confirmation required |
| CONNECTED | Press, activity, or advertising timeout | Remain CONNECTED; do not extend connection deadline |
| CONNECTED | Disconnect event OR connection deadline | READY_TO_SLEEP; never re-advertise, never force disconnect before sleeping |
| READY_TO_SLEEP | Any BLE callback, button, timer | Stay READY_TO_SLEEP; no reinit/re-advertise this wake |

No STOPPING, DISCONNECTING, controller/Host deinit, or post-client advertising. Security/pairing/auth callbacks keep their existing separate logic and must not override lifecycle READY_TO_SLEEP.

## Buzzer FSM transition completeness

| State/context | Event/condition | Result |
|---|---|---|
| IDLE | LOAD_ON physically applied for new startup | WAIT_SETTLE; deadline + 1000 ms |
| WAIT_SETTLE | Local deadline reached | MEASURING; request *fresh* INA voltage |
| MEASURING/STARTUP | Matching successful voltage | BEEPING normal or low-battery pattern based on current config |
| MEASURING/STARTUP | Matching error or bounded deadline | BEEPING exactly one triple-short error pattern |
| IDLE | New shutdown request | MEASURING/SHUTDOWN; INA sample while load ON |
| MEASURING/SHUTDOWN | Matching low voltage | BEEPING configured low-battery pattern |
| MEASURING/SHUTDOWN | Normal voltage, INA error, or deadline | IDLE; mark feedback complete, no error sound |
| IDLE | Authorized periodic warning | BEEPING warning pattern according to existing warning policy |
| BEEPING | Matching playback completion or playback deadline | Force buzzer pin OFF; IDLE; optional DONE |
| Any state | Stale/duplicate completion | Ignore; do not restart sound or extend deadline |

If warning playback collides with a mandatory startup/shutdown operation, mandatory sequencing wins; do not play overlapping sounds. The implementation must document whether it cancels or defers a warning, and test that the output never remains active beyond its deadline. Do not invent extra warning sounds.

## Power decision

* Without an initialized BLE session: after bounded periodic sampling, allow Deep Sleep with configured wake period.
* With BLE started: BLE state `READY_TO_SLEEP` permits Deep Sleep after bounded mandatory load/buzzer work; `STARTING`, `ADVERTISING`, or `CONNECTED` may not block beyond their respective deadlines.
* `LOAD_WORKING` **does not** veto sleep. Ordinary queue fullness, a missed timer notification, or lost BUZZER_DONE **does not** veto sleep.
* A **real** output GPIO failure is separate: keep physical state truthful and apply previously agreed hardware-fault handling. Do not implicitly convert hardware ERROR to a lost-event ERROR.
* The final Deep Sleep operation uses existing RTC output state and GPIO hold, with no controller shutdown, disconnect handshake or advertising stop acknowledgement.

## Implementation warning

A single synchronous event loop is sufficient for state logic. NimBLE callbacks can publish events without blocking. Keep a practical bound on event processing per iteration so a busy event stream cannot starve deadline evaluation and sleep. Deadline checks must still run if the event queue is perpetually nonempty.
