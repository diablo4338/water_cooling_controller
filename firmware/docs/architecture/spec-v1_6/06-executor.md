# Action execution and bounded fallbacks

Executor executes real functions for GPIO, INA, buzzer timer/waveform, NimBLE init/advertising and Deep Sleep. `command accepted` is not `hardware completed`. Do not synthesize success for failed GPIO. Avoid a separate architectural layer for controller shutdown because it is intentionally absent.

| Command | Expected result | On failure/no notification |
|---|---|---|
| LOAD_ON | GPIO call result | Limited retry/true output fault; no fake ON |
| LOAD_OFF | GPIO call result | Limited retry (existing policy), then explicit hardware ERROR; do not claim OFF |
| START_SETTLE | Timer or elapsed-time check | Deadline check drives next step without event |
| INA_SAMPLE | Real voltage/current or error | Existing bounded driver result; operation deadline fallback; never fake zero |
| BUZZER_PLAY | Audio physically toggles with configured timings | At playback deadline force pin inactive and continue |
| BUZZER_STOP | Pin inactive | Log failure; no waiting for DONE forever |
| BLE_START | Real NimBLE initialization/sync | Bounded startup failure → sleep eligibility; no second init this wake |
| BLE_ADVERTISE | Real advertising | Window deadline triggers sleep directly, no controller-stop ACK |
| ENTER_DEEP_SLEEP | ESP-IDF deep sleep entry | Preserve output hold and wake timer |

Every asynchronous operation has owner, ID and a timeout or elapsed-time based fallback. Timeouts should be measured by actual monotonic clock in the current wake, not by waiting for a queued timeout event. Event drops should be logged, not escalated into a sticky global sleep prohibition. Keep existing INA226 retry/delay budgets; no exact hard-real-time WCET guarantee is required.

## v1.6 — Load GPIO level verification is an executor operation
Keep the event-driven design and action executor. Introduce/reuse a bounded `LOAD_LEVEL_CHECK` command/result when a GPIO write completion event is absent. The executor performs `gpio_get_level()` on the output GPIO configured for reading, in the same serialized context as load GPIO writes; report pin, requested level, observed level, result status and operation ID to the Load FSM. A successful logical-level match permits completing the transition. `gpio_set_level()` returning success and a matching `gpio_get_level()` are software-level evidence only; neither proves actual output voltage, relay/MOSFET conduction, or powered load. Verify GPIO configuration/API semantics for ESP32-C3 before relying on readback; do not claim it is an independent physical feedback sensor. If command delivery also fails, use an independently checked bounded retry/deadline and preserve fault semantics rather than waiting forever.
