# Failure handling — per-operation, not global

| Failure | Required behavior | Sleep consequence |
|---|---|---|
| INA periodic sample fails | Load resets low-current count; no buzzer error pattern | Normal sleep after cycle |
| INA startup sample fails/timeouts | Exactly one triple-short error signal for that startup | After sound ends or deadline |
| INA shutdown sample fails/timeouts | Silent, continue physical LOAD_OFF | After bounded shutdown |
| Buzzer completion missing | At deadline force pin OFF, continue | Never blocks indefinitely |
| Timer event lost / timer creation failed | Poll stored deadline | No sticky sleep veto |
| Event queue full | Log/count dropped event; poll deadlines | No sticky sleep veto |
| BLE ready event lost/start fails | Bounded startup fallback | Sleep, not endless BLE ERROR |
| Advertising expiry | Sleep on deadline | Do not wait for GAP completion |
| Client disconnect | Sleep as soon as necessary bounded load/buzzer activity completed | No restart advertising |
| Connected 600s reached | Sleep directly, even if still connected | No disconnect attempt |
| Physical LOAD_ON/OFF API failure | Do not invent hardware success; finite retry/explicit load ERROR | Hardware failure may hold awake; distinct from lost event |
| RTC invalid | Default output OFF and clear retained data | Normal sleep if GPIO applied |

No global delivery fault latching; no exponential retry engine; no two-disconnect-retry logic; no BLE host/controller shutdown. Logs are useful but logging must not block event progress indefinitely.

## v1.6 — Distinguish missing notification from GPIO fault
A missing `LOAD_ON_DONE`/`LOAD_OFF_DONE` triggers a bounded logical GPIO level check, not automatic `LOAD_FSM_ERROR`. Matching level => finish pending state transition; mismatch => bounded retry; failed/unavailable level read or persistent mismatch => physical/control fault policy. Never use `gpio_get_level()` as evidence of actual power delivered to the lamp. Do not add a global sleep veto merely because an event was lost.
