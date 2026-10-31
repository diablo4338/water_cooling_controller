# 02 — Load FSM contract

States: `LOAD_IDLE` (output OFF); `LOAD_STARTING` (output ON, startup sequence in progress); `LOAD_WORKING` (output ON); `LOAD_STOPPING` (output ON, shutdown completion pending). Adding transient states avoids reporting IDLE while output is still ON.

| State | Event/guard | Next state | Commands |
|---|---|---|---|
| IDLE | THERMAL_BUTTON_PRESSED | STARTING | `LOAD_OUTPUT_ON`; retain desired state; initiate startup only after physical ACK |
| STARTING | LOAD_GPIO_APPLIED(ON) | STARTING | publish STARTUP request to Buzzer FSM (wait 1000 ms then fresh INA read) |
| STARTING | BUZZER_DONE(STARTUP) | WORKING | No GPIO change; make working state durable |
| STARTING | LOAD_GPIO_FAILED(ON) | IDLE or ERROR | Fail safely; signal/log hardware error; no fake successful activation |
| STARTING | THERMAL_BUTTON_PRESSED | STARTING | Ignore duplicate |
| WORKING | THERMAL_BUTTON_PRESSED | WORKING | No load change (BLE separately handles press) |
| WORKING | valid `CURRENT_LOW` first sample | WORKING | Begin low-current interval |
| WORKING | valid `CURRENT_LOW`, elapsed < configured limit | WORKING | Maintain low-current interval |
| WORKING | valid `CURRENT_HIGH` | WORKING | Reset pending low-current timer |
| WORKING | `INA_SAMPLE_FAILED` | WORKING | Reset low-current pending; **no tone** |
| WORKING | valid low-current timeout | STOPPING | `REQUEST_BUZZER_SHUTDOWN`, keep output ON |
| STOPPING | `BUZZER_DONE(SHUTDOWN)` matching id | STOPPING | `LOAD_OUTPUT_OFF` |
| STOPPING | `LOAD_GPIO_APPLIED(OFF)` | IDLE | Persist state, permit sleep if otherwise safe |
| STOPPING | button or periodic readings | STOPPING | No duplicate shutdown, don't reactivate |

**Time policy:** keep existing configurable current threshold and low-current timeout (presently 100 mA and 300 s defaults), subject to firmware config. Current sample used on activation was collected *before* changing output; do not apply it to prove new loaded operating state. Keep fail-safe rule: invalid current never counts toward timeout, instead resets low-current timer as current behavior does.

**Power/RTC:** WORKING is allowed to enter Deep Sleep; output hold must keep it on. STARTING and STOPPING are *not* sleepable. On wake, restore output and durable working/idle state. Do not retain transient timer-dependent states across sleep unless explicit recovery is designed; avoid entering sleep in those states. Ensure gpio hold behavior and RTC validation remain correct.

**Shutdown after current timeout:** a shutdown voltage request happens while output remains physically ON. If that request fails/times out, Buzzer FSM finishes shutdown silently; Load eventually turns output OFF. No infinite wait. A separate optional warning tone is subject to approved policy, not to be invented.
