# Event protocol and non-blocking delivery

## Envelope
At minimum: `kind`, `operation_id` or `request_id` where asynchronous completion must be correlated, context (`PERIODIC`, `STARTUP`, `SHUTDOWN`, `WARNING`), result/error and data. A timestamp is optional where needed for deadline comparisons. IDs reset on boot; results from the wrong ID or previous operation are ignored.

| Producer | Event | Consumers | Notes |
|---|---|---|---|
| Thermal detector | `THERMAL_BUTTON_PRESSED` | Load, BLE | One edge shared by both |
| Scheduler | `PERIODIC_SAMPLE` / deadline tick | Load, Buzzer, BLE/power | Deadlines checked from monotonic clock in same boot |
| Load GPIO executor | `LOAD_APPLIED`, `LOAD_FAILED` | Load, Buzzer | Real execution result, not queue acceptance |
| Settle timer/deadline checker | `SETTLE_ELAPSED` | Buzzer | Optional notification; deadline check always exists |
| INA226 adapter | `INA_OK`, `INA_FAILED` | Load/Buzzer by context | Never interpret failure as 0 current or low battery |
| Buzzer executor | `BUZZER_DONE`, `BUZZER_FAILED` | Buzzer, Load for shutdown | Optional completion; deadline fallback mandatory |
| NimBLE host | `BLE_READY`, `BLE_FAILED` | BLE | Startup may time out |
| GAP | `CLIENT_CONNECTED`, `CLIENT_DISCONNECTED` | BLE | Disconnect means sleep eligibility immediately |

## Delivery policy
Publish must not block. A full queue is observable (counter/log) and may drop an event. **A dropped event alone is not a sleep blocker**. State decisions must not depend on guaranteed event arrival: timeout/deadline checks run independently each runtime step. Prioritize real hardware state/timeout when a completion was dropped. For load ON/OFF uncertainty, do not pretend hardware succeeded; apply a bounded hardware action policy. No global sticky delivery fault; no reserved overflow queue or attempt at exact cross-queue FIFO.

## Ordering rules
Dispatcher processes received events sequentially; each FSM ignores irrelevant events. For one operation, completion and deadline racing must resolve exactly once via operation ID and terminal flag. A late completion does not replay a sound, restart BLE or undo load-off. BLE deadline is checked even if `CLIENT_DISCONNECTED` event is lost; absence of a connection indication does not extend beyond the 600-s cap.

## Non-blocking requirement
Never wait for `BUZZER_DONE`, `INA_OK`, GAP callbacks, queue capacity or timer notification with unbounded `portMAX_DELAY`. All waits have per-operation time caps or are purely asynchronous. This is not a promise that every GPIO/I2C API is instantaneous; preserve existing bounded hardware driver operations.
