# ESP32-C3 runtime FSM architecture — contract v1

Status: **approved functional requirements**, implementation pending. Source baseline: `firmware(1).zip` supplied 2026-10-30. This specification is the authority for the intended refactor; current source remains authority for existing hardware pinout, BLE security/pairing, and configuration defaults unless explicitly overridden here.

Read in order:
1. `01-events-and-ownership.md`: components, event schemas, delivery and time.
2. `02-load-fsm.md`: transitions and output sequencing.
3. `03-ble-fsm.md`: lazy BLE startup and shutdown.
4. `04-buzzer-fsm.md`: voltage sampling, patterns, and error policy.
5. `05-power-and-integration.md`: coordinated scenarios, sleep and retained state.
6. `06-acceptance-tests.md`: mandatory tests and acceptance criteria.

## Non-negotiable rules
- **Three FSMs:** Load, BLE and Buzzer. Event dispatcher routes messages; hardware/action executor performs side effects; Power Manager alone decides Deep Sleep.
- Load and BLE independently receive a **new thermal-button press**. Do not conflate with the physical GPIO19 pairing/reset button.
- Start without initializing BLE unless a button event opens a window. Idle wake: take measurements then sleep; preserve 5-second configurable sleep behavior.
- Load output remains powered during Deep Sleep if `WORKING`; `WORKING` does **not** block sleep.
- On load enable: assert output **first**, wait **1000 ms**, request a **fresh** INA226 measurement, select signal and play it.
- On load disable: while output is **still on**, obtain fresh voltage (or handle read failure), run any configured shutdown signal, **then** deassert output. Do not infer voltage from an earlier pre-transition sample.
- A failed INA226 measurement becomes an event; it must **never** be interpreted as zero/low current or low battery.
- **Only a startup/button-triggered measurement error** produces one **three-short-beep pattern**. Periodic errors are silent. Shutdown measurement errors never produce the three-beep error pattern and must not prevent completing shutdown.
- BLE `CONNECTED + CLIENT_DISCONNECTED → STOPPING` **unconditionally**. Do not advertise again on disconnect, even if a window remains.
- No infinite timer waits or duplicate signals from duplicate/late results. Do not enter Deep Sleep with pending load transitions, buzzer activity, or an active BLE session.
- Do not change pairing credentials, security behavior, protocol, public API, thresholds or hardware pin assignments incidentally.

## Open decisions, not permission to improvise
- Exact shutdown sound policy is **not yet approved**: do not invent a new sound. Preserve existing behavior where possible; if no shutdown beep existed, the shutdown sequence may be silent while still waiting for a fresh voltage result before switching off. Report this explicitly.
- The three-error-beep pattern should be configurable or centralized; proposed baseline 3 × 150 ms with 150 ms gaps; do not silently change existing normal/low/combined durations.
- Details of ESP-IDF/NimBLE host start/stop sequence depend on actual `idf.py --version`. Check installed SDK APIs, especially repeated controller disable/enable; **do not** call controller `deinit()` or memory release in a repeatable awake session.
