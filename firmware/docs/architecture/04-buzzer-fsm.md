# 04 — Buzzer FSM and INA errors

States: `IDLE`, `WAIT_SETTLE`, `MEASURING`, `BEEPING`; completion is `BUZZER_DONE(context, operation_id)`, not necessarily a persistent DONE state. Keep Buzzer operation `STARTUP`, `SHUTDOWN`, `WARNING` separate from phase state.

| State | Event | Next/action |
|---|---|---|
| IDLE | `LOAD_GPIO_APPLIED(ON)` for startup | WAIT_SETTLE; start 1000 ms timer |
| WAIT_SETTLE | SETTLE_ELAPSED matching op | MEASURING; request fresh INA226 voltage (STARTUP context) |
| MEASURING, STARTUP | INA_SAMPLE_OK matching op | BEEPING; choose *existing configured* normal startup vs low-battery combined tone from fresh loaded voltage |
| MEASURING, STARTUP | INA_SAMPLE_FAILED/timeout matching op | BEEPING; **one three-short-beep ERROR pattern**, not low-voltage tone |
| IDLE | `REQUEST_BUZZER_SHUTDOWN` | MEASURING; take fresh INA226 voltage before switching OFF |
| MEASURING, SHUTDOWN | INA_SAMPLE_OK matching op | BEEPING if a *previously approved* shutdown signal exists, otherwise finish SHUTDOWN silently |
| MEASURING, SHUTDOWN | INA_SAMPLE_FAILED/timeout | IDLE; finish SHUTDOWN silently; do not block load OFF |
| IDLE | confirmed sustained low voltage while WORKING | BEEPING; one warning according to existing configurable low-delay policy |
| IDLE | INA_SAMPLE_FAILED in PERIODIC context | IDLE, **silent** |
| BEEPING | BEEP_FINISHED matching pattern id | IDLE; emit BUZZER_DONE for STARTUP/SHUTDOWN as applicable |
| WAIT_SETTLE/MEASURING/BEEPING | unrelated/duplicate/late sample | unchanged, ignore |

Low-voltage classification **only** from finite valid voltage. Failed voltage measurement is unknown, not low. Do not clear existing low-voltage warning state merely because sample failed (unless documented, deliberate policy); maintain threshold and delay functionality across RTC wake cycles, subject to test. While STARTUP/SHUTDOWN active, periodic warnings must not preempt the sequence. Shutdown has priority over routine warning; do not stack two beeps.

**Error pattern:** propose 3 × 150 ms ON separated by 150 ms OFF, emitted **once per STARTUP operation id** (not once per INA read and not once per boot). Keep normal `button_ms`, `low_ms`, `combined_ms`, `buzzer_enabled` and threshold settings. If buzzer disabled, no audio, but STARTUP/SHUTDOWN must still complete via events. Error pattern must have finite total duration and completion ACK. Do not drive buzzer through blocking `vTaskDelay()` on event worker.

**Key distinction:** periodic sample failures cause no beep, even if repeated indefinitely. STARTUP-triggered measurement failure causes exactly three pulses once, even if same error event is delivered twice. A *new separate button startup* may cause another pattern if sensor still broken.

**Boundary note:** existing implementation has *no distinct shutdown tone*. Do not create one without approval. 'Check buzzer before switching off' here means perform and complete the shutdown buzzer decision (including its voltage read and potential already-specified tone) with output still ON. Default approved behavior remains silent shutdown if there is no defined tone.
