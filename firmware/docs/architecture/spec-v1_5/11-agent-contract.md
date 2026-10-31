# Implementation agent contract — architecture v1.4

1. **Read all documents, especially `14-transition-coverage.md`,** in this package; old v1.0–v1.2 are superseded. Source repository is AS-IS; this specification is TO-BE. Highlight unresolved conflicts before editing.
2. The critical rule: **events are useful, not reliable prerequisites for progress**. Any missing DONE or callback is handled by local deadlines. Do not add sticky delivery-error sleep gates, sophisticated replay/overflow queues, or infinite waits.
3. BLE: initialize only on thermal press, once per wake; no controller deinit/disable/stop; advertise up to 60s; on client disconnect sleep; at 600s connected sleep directly, without disconnect calls or confirmation.
4. Buzzer: GPIO ON → >=1000ms → fresh INA → sound. At shutdown INA check → low battery sound only → GPIO OFF. If BUZZER_DONE missing, sound deadline stops buzzer and proceeds. INA startup failure: three distinct beeps only once; periodic failures silent.
5. Load WORKING may sleep. RTC valid → restore; RTC invalid → default OFF. Preserve existing INA226 driver budgets, config, pairing, GPIO19 behavior and hardware pin mapping.
6. Use small increments and RED/GREEN tests; no fake success actions, mocks in production, silent threshold changes, or broad unrelated rewrites. Report exact test and build commands. Real hardware behavior remains unverified until tested.
7. Propose implementation changes **only when authorized**. This spec is not authorization. When source contradicts spec, write explicit deviation instead of changing the spec to fit source.
