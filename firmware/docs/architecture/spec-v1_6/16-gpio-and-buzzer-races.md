# GPIO acknowledgment fallback and buzzer timer callback contract (v1.6)

Status: **APPROVED clarification**. This supplements v1.5 without changing the agreed three FSMs, serialized events, action executor, buzzer patterns, NimBLE lifecycle, or Deep Sleep policy.

## A. Why the GPIO completion event can be lost

The Load FSM issues an ON/OFF command to the action executor. The executor calls `gpio_set_level()` and publishes a result. The output may already be at the requested level even if the event never reaches the FSM. Missing acknowledgment is *not* evidence of a failed GPIO write. Do not keep the CPU awake indefinitely on that basis.

## B. Required GPIO flow

| Condition | Required behavior |
|---|---|
| GPIO write acknowledgment arrives and succeeds | Complete the corresponding Load FSM transition for the matching operation ID |
| Acknowledgment deadline expires | Ask executor to inspect `gpio_get_level()` for that load output, ordered with writes |
| Reported logical level matches requested level | Complete the pending transition; ignore any later redundant acknowledgment |
| Logical level differs | Retry the GPIO write within bounded existing retry budget; re-evaluate at deadline |
| Level check fails, or mismatch persists after retries | Explicit load/output fault; never assert `IDLE` if OFF cannot be established |
| Old result belongs to superseded operation | Ignore it |

GPIO readback must be supported by actual pin configuration and ESP-IDF behavior. It observes the *logical output level*, not independent electrical feedback from the power path. Never claim it proves that a transistor, lamp or supply has actually switched.

The read/compare/retry runs through the existing action executor + events. No direct Load FSM GPIO access and no new architecture layer. Use existing independent deadline polling; the fallback itself must not depend on a missing completion event.

## C. Buzzer stale callback — precise scenario

1. Buzzer pattern **A** is playing, and its timer callback is queued or about to run.
2. Pattern A is cancelled; the timer is stopped.
3. New pattern **B** starts; the same callback handler may inspect *current* global Buzzer playback state.
4. A callback associated with A executes late. If it blindly advances the current phase, it may shorten a phase of B.

The current implementation guards against callbacks earlier than the *current* phase deadline. This check alone does not identify callback ownership if an old callback arrives at or after B's current deadline. **This is a potential ordering issue, not proof of a real bug.** Determine if that exact sequence can occur in the deployed ESP-IDF `esp_timer` task/dispatch mode and this code's timer start/stop order.

## D. Required buzzer policy

- Keep the existing `esp_timer`, FSM, executor, operation IDs and phase deadlines.
- `CANCEL(A)` must not cancel `B`; cancellation must be scoped by operation ID.
- An old callback must not advance a newer pattern if it is distinguishable or can be shown to be reachable. Test real behavior with production logic and appropriate timer harness.
- If real ESP-IDF serialization rules preclude the suspected ordering, document the rule/source, retain the existing implementation and test the actual boundary cases.
- Do **not** add a new scheduler, timer wheel, new FreeRTOS task, or replace callbacks with 250-ms polling just for this hypothetical race.
- Existing finite playback deadline and `BUZZER_DONE`-loss fallback remain in force; no indefinite wait or global sleep lock.

## E. Tests and change control

Write targeted host tests for lost ON/OFF acknowledgments with matching/mismatching `gpio_get_level`, stale acknowledgment and stale cancel. Investigate callback A/B timing against the actual ESP-IDF version. Add a RED test before any production change for a confirmed callback defect. Changes must remain small and must not alter BLE, pairing, INA conversion budgets, thresholds, RTC behavior, or GPIO assignments.
