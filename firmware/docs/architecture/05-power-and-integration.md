# 05 — Integration, wake/sleep, chronological traces

## Boot without button
1. Cold/Deep Sleep wake; restore RTC Load state, held GPIO; set BLE OFF and Buzzer IDLE.
2. Initialize sensors and execute one measurement cycle. Route valid/failed INA result to Load FSM; failed periodic read is **silent**.
3. If no BLE window and no transitional load/buzzer work: hold output and enter configured Deep Sleep (baseline 5000 ms); no NimBLE init anywhere on this path.

## Thermal button when LOAD_IDLE
1. Detector emits one `THERMAL_BUTTON_PRESSED` to Load and BLE.
2. Load commands ON; BLE requests lazy start/open 60 s window independently.
3. On confirmed output ON, Buzzer WAIT_SETTLE for 1000 ms.
4. Fresh INA voltage: OK -> existing normal/combined startup sound; FAIL/timeout -> **exactly three short beeps**.
5. `BEEP_FINISHED` -> Buzzer IDLE; Load STARTING -> WORKING.
6. Do not sleep while action pending, even if BLE never came up.

## Low-current shutdown while load ON
1. Only valid continuous low-current readings for configured timeout may trigger shutdown.
2. Load enters STOPPING and keeps output ON.
3. Buzzer requests fresh INA voltage before OFF; missing/failed sample does **not** play error pattern. Conclude cue (silently by default).
4. Load receives matching `BUZZER_DONE(SHUTDOWN)` -> commands output OFF; verify hardware acknowledgement -> IDLE.
5. Sleep permitted once BLE is OFF and no critical pending actions.

## BLE when working load
1. BLE window expires after 60 s, no connected client -> STOPPING, host/controller radio off -> BLE OFF.
2. WORKING load persists on output hold; device enters periodic Deep Sleep if otherwise eligible.
3. If client connects before window expiry: stay CONNECTED even after 60 s. On disconnect -> STOPPING **immediately**, never re-advertise automatically.
4. Button in STOPPING requests BLE restart after the stop finishes. Button in CONNECTED has no BLE effect; it also does not toggle working load.

## Deep Sleep gate

```
can_sleep = (BLE == OFF)
         && (LOAD != STARTING && LOAD != STOPPING)
         && (BUZZER == IDLE)
         && (no in-flight must-complete GPIO/INA/buzzer operation)
         && (no queued actionable events that must be applied before sleep)
```

`LOAD_WORKING` is sleepable. Explicitly serialize the final gate with event consumption and publication. Gate-time
task/ISR events are retained and abort sleep; queue overflow uses a bounded staging
ring and an unstoreable event raises a sticky fault that blocks sleep. This closes
the queue-empty/consumer-local-event race. The final pending/abort check is repeated
after wake configuration/output hold. It cannot be atomic with
`esp_deep_sleep_start()`: an event after that last check is safe only if its source is
a configured Deep Sleep wake source. The GPIO19 button is configured as a wake
source; the code makes no check-and-sleep atomicity claim for other sources.
Store only required RTC stable fields; avoid NVS write each wake. Power measurements:
duration of each wake, sleep current, wake peak, average over entire cycle; 6 mA
*peak* alone cannot diagnose BLE.

## INA226 execution bound

The action worker calls `ina226_get_single_sample_ex()` synchronously. Each bus-lock
wait and each I2C transfer is capped at 100 ms. A one-shot conversion polls status
at most 40 times with 10 ms delays; there are no retry loops. A first-use sample may
also add the device, configure/verify three registers, then start conversion and
read bus/current. With every lock and I2C transfer reaching its full timeout and
readiness arriving on the final poll, the expected successful upper path is 10.3 s.
A final current-register timeout plus recovery bus reset raises this to at most
10.45 s on the installed
ESP-IDF 5.4.4 implementation (the I2C bus-clear routine is capped at 50 ms).
These API timeout budgets exclude scheduler jitter and FreeRTOS tick granularity.

The 1.5 s measurement timer is an FSM deadline, not a cancellation mechanism: it
can transition the Buzzer FSM while the action worker is still blocked. The
dispatcher is a separate task and continues processing events; Deep Sleep remains
blocked by the busy action worker until the bounded INA call returns. Host timing
tests exercise the production INA adapter against the documented timeout limits;
they do not establish real-hardware bus timing.

## Event interleavings to design for
- stale `WINDOW_EXPIRED` after repeated press extends deadline;
- `CLIENT_CONNECTED` just as advertisement times out;
- duplicate CLIENT_DISCONNECTED or late callback after BLE stop;
- STARTUP INA response arrives after a timeout/fallback and a newer operation starts;
- INA failure exactly at low-current cutoff;
- Deep Sleep requested during beep or unsettled output;
- power loss / watchdog reset in transient state;
- GPIO19 physical reset/pairing while thermal startup is active.
