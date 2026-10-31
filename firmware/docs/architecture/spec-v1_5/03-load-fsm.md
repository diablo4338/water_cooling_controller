# Load FSM — normative transition contract

States: `IDLE` (output commanded OFF), `STARTING` (ON requested / startup feedback pending), `WORKING` (output ON), `STOPPING` (shutdown feedback pending), `ERROR` (actual physical output failure, not queue failure). `ERROR` is **not** entered merely because a completion event was dropped.

| From | Event/guard | To | Action | Deadline fallback |
|---|---|---|---|---|
| IDLE | thermal press | STARTING | Request GPIO ON, record operation | If no confirmation, inspect/attempt bounded GPIO action, never claim ON without execution |
| STARTING | ON applied | STARTING | Trigger buzzer STARTUP sequence; mark load ON | Buzzer deadline ends startup feedback |
| STARTING | startup feedback completed or timed out | WORKING | No GPIO change | Ignore late BUZZER_DONE |
| STARTING | physical GPIO ON failed | ERROR | Log true hardware fault | Remain awake if unable to establish intended state |
| WORKING | thermal press | WORKING | Do not toggle load | BLE independently receives press |
| WORKING | valid current below configured threshold | WORKING | Track low-current timer | Persist safe timer state across Deep Sleep |
| WORKING | valid current at/above threshold | WORKING | Clear low-current timer | — |
| WORKING | INA periodic failure | WORKING | Reset low-current timer, do not treat as zero current | Periodic errors silent; normal sleep allowed |
| WORKING | low-current deadline reached | STOPPING | Keep GPIO ON; request Buzzer SHUTDOWN | On missing buzzer result, force sound OFF and proceed to GPIO OFF |
| STOPPING | shutdown feedback completed or deadline | STOPPING | Command actual GPIO OFF | Missing buzzer DONE never prevents OFF |
| STOPPING | OFF applied | IDLE | Save OFF to RTC | Ignore duplicate completion |
| STOPPING | actual GPIO OFF failed | ERROR or bounded retry | Log/attempt existing limited retry policy | Cannot falsely claim OFF |

The Load FSM owns physical load requests; Buzzer FSM never writes load GPIO. `LOAD_WORKING` is allowed to Deep Sleep. State and relevant low-current history retained across wake. Startup feedback is **not** repeated merely because RTC restored an already-ON output. Thermal button activation while working does not turn output off.

At a startup-feedback deadline the load stays ON; at a shutdown-feedback deadline physically stop buzzer and proceed to OFF. Timers and signal durations must come from existing config or be documented design constants; do not add infinite waiting states.
