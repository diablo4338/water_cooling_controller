# Components and responsibilities

1. **Thermal detector / ADS1115** identifies a qualified *edge* and emits `THERMAL_BUTTON_PRESSED` once per press, not every sample.
2. **INA226 driver** returns real sample or error. It does not command load or buzzer. Do not change the existing bounded driver timing/retries for architectural reasons.
3. **Load FSM** manages desired/confirmed output, low-current shutdown timer, and start/stop ordering. Restorable across Deep Sleep.
4. **BLE lifecycle FSM** manages single on-demand NimBLE initialization, advertising window and connection deadline. Distinct from existing pairing and authorization FSMs.
5. **Buzzer FSM** controls settling, requested voltage sample, sound pattern and completion/timeouts. No retained state across Deep Sleep.
6. **Dispatcher** routes events serially in a bounded, non-blocking way; does not wait for acknowledgments. Prefer a simple central pump; a bounded queue is acceptable, but no secondary reliable-delivery system.
7. **Executor** calls actual GPIO, timer, INA, NimBLE, buzzer APIs, returning results when available. No fake success events.
8. **Power coordinator** evaluates timed BLE rules and pending physical sequences and enters Deep Sleep. Never calls `esp_bt_controller_disable/deinit` or NimBLE deinit as part of ordinary sleep.
9. **RTC restore** uses retained load value when valid; otherwise applies default OFF. Do not build extra elaborate held-GPIO reconciliation logic.

Event → FSM transition → requested command → executor → optional result event. **Every transition waiting for a result has an independently checked deadline**. Poll deadlines even if queue delivery fails. An operation deadline may end or abort an operation according to its local fallback rather than a global `ERROR`.

Hardware-related callbacks must not run unrelated FSM reducers recursively. They may enqueue, set simple notifications, or update atomic flags; the main runtime performs serialized progression. No separate task per FSM.
