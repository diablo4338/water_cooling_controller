# Boundary rules: one terminal outcome, deadlines prevail

- Every operation can terminate by completion **or** deadline. Once terminated, matching late events are ignored; no duplicate beeps, GPIO toggles or BLE starts.
- Completion event may make transition earlier; no event can postpone a hard deadline. A dropped event cannot create an indefinite sleep veto.
- On startup, physical LOAD_ON must be executed before 1000-ms settling. Do not measure voltage from earlier periodic sampling.
- On shutdown, if BUZZER_DONE is missing by playback deadline, force buzzer output OFF and issue LOAD_OFF. Missing DONE alone is not an error condition that blocks sleep.
- On low battery during shutdown, beep only with configured threshold/pattern. On INA error during shutdown, remain silent. During periodic sampling, INA errors are silent and reset low-current timer.
- A real GPIO hardware failure cannot be converted into fake success merely because deadline elapsed. Handle output uncertainty using existing bounded attempts and an explicit load fault.
- A BLE client disconnect and 600-s deadline occurring together both choose READY_TO_SLEEP. Never restart advertising. If connection persists at deadline, enter Deep Sleep anyway.
- A button during CONNECTED does not extend time. A button after BLE is READY_TO_SLEEP does not start a second BLE lifecycle before the next boot.
- At advertising window deadline, sleep without needing NimBLE stop confirmation. Controller stays initialized right until sleep.
- No global event delivery lock/reliable queue/fatal overflow policy is necessary. Use regular serialized dispatch and independently polled deadlines.
- Retained Load valid → restore desired output; invalid → OFF default. The thermal detector is sampled, not a magically guaranteed hardware wake pin.

## v1.6 — GPIO and buzzer boundary conditions
- Missing GPIO acknowledgment is **not** equivalent to failed GPIO output. On deadline verify logical output level via executor, preserving serialized writes; match completes transition; mismatch follows bounded retry/fault. Readback is not independent electrical/load verification.
- Delayed buzzer timer callback belongs to an earlier operation: never let it shorten or advance a newer pattern. Existing operation ID and phase deadlines are retained; establish whether the harmful ordering is reachable in actual ESP-IDF `esp_timer` callback semantics before adding mitigation. Do not redesign proactively.
