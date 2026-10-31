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

`LOAD_WORKING` is sleepable. Explicitly serialize final gate with BLE callbacks/event publication; avoid check-then-sleep race. Store only required RTC stable fields; avoid NVS write each wake. Power measurements: duration of each wake, sleep current, wake peak, average over entire cycle; 6 mA *peak* alone cannot diagnose BLE.

## Event interleavings to design for
- stale `WINDOW_EXPIRED` after repeated press extends deadline;
- `CLIENT_CONNECTED` just as advertisement times out;
- duplicate CLIENT_DISCONNECTED or late callback after BLE stop;
- STARTUP INA response arrives after a timeout/fallback and a newer operation starts;
- INA failure exactly at low-current cutoff;
- Deep Sleep requested during beep or unsettled output;
- power loss / watchdog reset in transient state;
- GPIO19 physical reset/pairing while thermal startup is active.
