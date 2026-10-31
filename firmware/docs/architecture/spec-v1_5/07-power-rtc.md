# Power, RTC, and Deep Sleep

## Decision
Normal wake without BLE activation: after bounded periodic sampling and load handling, sleep with configured 5-s interval. If BLE activated: sleep after advertising window expires with no connection, after client disconnects, or **at 600 s from connection** even if client is still connected. Those are conditions for sleep; pending bounded load/buzzer physical sequences complete or expire via fallback first. `LOAD_WORKING` allows sleep. **Neither lost events nor a failed queue automatically veto sleep.** No routine controller/host stop, disable, deinit or disconnect before sleep.

## RTC recovery
If retained load state is valid, restore requested output state. Otherwise set default load OFF, clear retained state. No complicated comparison between RTC and GPIO hold. Preserve the original GPIO hold mechanisms and wake sources. Deep Sleep resets ordinary NimBLE/Buzzer state, and next wake starts BLE OFF. Keep correct low-current time across Deep Sleep using the current working approach; do not use a raw previous-boot `esp_timer_get_time()` value as valid elapsed time.

## Operations in progress
A startup settle or buzzer playback may hold off sleep only until its finite deadline; failure to deliver completion triggers fallback, never infinite waiting. A real LOAD_OFF GPIO failure is distinct: do not claim OFF; according to the chosen physical-fault policy it may remain awake for diagnosis/recovery. BLE window/connection hard deadlines must be independently checked, regardless of dispatcher state.

## Implementation note
Deep Sleep can interrupt a currently connected BLE client; this is intended. Do not add disconnected/retry states to make the shutdown graceful. Before entering sleep, perform the minimal RTC and GPIO-hold preparation already required by hardware. No controller-off confirmation required.
