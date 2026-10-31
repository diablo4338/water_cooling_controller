# Buzzer FSM — startup, shutdown and warnings

States: `IDLE`, `WAIT_SETTLE`, `MEASURING`, `BEEPING`. Operation contexts STARTUP / SHUTDOWN / WARNING. No indefinitely sticky `ERROR` required for missing completion; local deadline provides fallback. Each operation has unique ID; stale or duplicate results ignored.

| State/context | Trigger | Next | Action / fallback |
|---|---|---|---|
| IDLE | physical load ON applied for new startup | WAIT_SETTLE | Set `settle_deadline = now + 1000 ms` |
| WAIT_SETTLE | deadline reached | MEASURING | Request fresh INA voltage; do not reuse preceding periodic sample |
| MEASURING STARTUP | matching INA success | BEEPING or IDLE | Normal button sound vs low-battery combined pattern using existing configured threshold/durations |
| MEASURING STARTUP | matching INA error or local measurement deadline | BEEPING or IDLE | Exactly one pattern of three short distinct beeps; never per periodic error |
| IDLE | shutdown requested | MEASURING | Fresh INA voltage while load still ON |
| MEASURING SHUTDOWN | sample below configured battery threshold | BEEPING | Only then play configured low-battery signal |
| MEASURING SHUTDOWN | normal voltage / INA failure / measurement timeout | IDLE | Silence, report shutdown feedback complete (or simply permit deadline) |
| IDLE | periodic low-voltage warning policy satisfied | BEEPING | Existing warning policy and durations |
| BEEPING | playback ended | IDLE | Force buzzer output OFF, emit optional BUZZER_DONE |
| BEEPING | playback deadline reached but no DONE | IDLE | **Force buzzer output OFF**, permit dependent load transition without notification |

If timer arming fails, check elapsed time in normal runtime step; no indefinite wait. If measurement fails at STARTUP, triple beep exactly once per operation; at SHUTDOWN, do not play error triple. If output/timer action fails, log and progress according to finite deadline; do not keep awake forever for sound alone. Sound durations and battery threshold are read from current config, not invented constants (apart from explicitly documented three-short-pulse error pattern parameters). If buzzer is disabled, finish sequencing silently.

Load output ON must precede WAIT_SETTLE; load output OFF occurs only after shutdown signal completion **or bounded timeout**. The buzzer never directly changes load GPIO. An ina periodic error must never trigger the triple error pattern.
