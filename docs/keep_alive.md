# BLE client keep alive

The authenticated Android client writes `01` (protocol version 1) with response
 to main-service characteristic `f5024a72-58bc-4f33-a778-cf6507593bea`.
Only that explicit write extends the connection deadline; metric reads,
notifications and configuration writes do not.

The existing 600-second connection timeout starts at GAP connect. Each accepted
keep alive extends it by 600 seconds. Unauthorized connections, invalid payloads,
other connection handles and expired sessions cannot extend it. Expiry follows
the existing BLE shutdown/disconnect path. Older clients still time out normally;
the updated Android client requires firmware exposing this characteristic.

Android sends within the watchdog's next one-second iteration after becoming
active, then every 30 seconds. Activity must be resumed, its window must have
focus, and PowerManager.isInteractive must be true. Leaving the app or turning
off the screen stops renewal; the controller disconnects 600 seconds after the
last renewal. No wake lock or foreground service is needed. GATT operations share
a coroutine Mutex so configuration writes and keep alive cannot overlap.
CancellationException propagates from the keep alive coroutine.

The NimBLE callback validates authorization and payload and publishes an event.
The existing runtime dispatcher owns the deadline, using its monotonic ESP Timer
clock; no extra task, timer or state machine was added. An ATT success means the
event was queued; an already expired runtime session cannot be revived.

References:
- [ESP-IDF ESP Timer](https://docs.espressif.com/projects/esp-idf/en/v5.1.7/esp32/api-reference/system/esp_timer.html): monotonic microsecond time via esp_timer_get_time.
- [Android window focus](https://developer.android.com/reference/android/app/Activity#onWindowFocusChanged(boolean)): focus is independent of lifecycle callback ordering.
- [Android PowerManager](https://developer.android.com/reference/android/os/PowerManager#isInteractive()): interactive device state.
- [Kotlin coroutine cancellation](https://kotlinlang.org/docs/cancellation-and-timeouts.html): cooperative cancellation and CancellationException propagation.
- [Kotlin shared mutable state](https://kotlinlang.org/docs/shared-mutable-state-and-concurrency.html): Mutex for coroutine mutual exclusion.

Validation: firmware host suite includes renewal, wrong-handle rejection and
expiry-boundary rejection. On hardware, leave the focused app connected for over
10 minutes, then background it or turn off the screen and verify disconnection
10 minutes after the last renewal. Repeat with focus changes, configuration
saving, reconnect, and app termination.

## Diagnosing disconnects

NimBLE reason 0x213 is HCI 0x13 (Remote User Terminated Connection),
with the NimBLE HCI error base 0x200. On the controller this points to the
phone terminating the connection. READY_TO_SLEEP after DISCONNECTED is expected.
See [Espressif BLE troubleshooting](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/ble-application-note/troubleshooting.html).

Android logs to tag BleController: watchdog/keep-alive failures, operation UUID,
GATT status, timeouts, explicit disconnect and Activity destruction. Capture with
`adb logcat -v time -s BleController BluetoothGatt AndroidRuntime`.
Watchdog reads now use the same eight-second timeout as other GATT operations.
A normal coroutine cancellation propagates; a GATT operation timeout is reported
and closes the session, since allowing a late callback to complete a later
operation would be unsafe. A keep-alive timeout must not silently stop the
watchdog (TimeoutCancellationException is also a CancellationException).

## Saving configuration under BLE buffer pressure

NimBLE rc=6 is BLE_HS_ENOMEM (resource exhaustion), not a configuration validation
error. A log with status=0 and Runtime config committed means NVS saving succeeded.
The result notification is now queued on the existing NimBLE host event queue
instead of being sent inside the ATT write callback. The queue callback checks
connection authorization again and does not retry or block the host on failure.
The result remains readable even when its notification cannot be sent.

After the write response, Android reads CONFIG_RESULT if that request is still
pending. Both read and notification use the existing requestId check, so a stale
result cannot confirm a different save. No second flash write is performed.
Operation timeouts are logged separately from a missing save confirmation.
References: [NimBLE error codes](https://github.com/apache/mynewt-nimble/blob/master/nimble/host/include/host/ble_hs.h)
and [notification mbuf ownership](https://mynewt.apache.org/latest/network/ble_hs/ble_gatts.html).

Hardware validation: save parameters repeatedly during live telemetry, verify
confirmation and uninterrupted BLE, reconnect and read the saved values. Also
verify read-based confirmation when a result notification fails with rc=6.

## Tracing a missing configuration write response

Firmware prints `Config diagnostics v2` during lazy BLE initialization. At the
write callback entry it prints `Config write received`, connection/attribute,
payload length and free mbuf count, before authorization or NVS calls.
Android logs requestId, submitted UUID/attribute instance/length/API status,
and the write callback UUID/instance/status.

Absence of `Config request` alone does not prove that the radio did not receive
the request. In ESP-IDF v5.4.4 NimBLE's ble_att_svr_rx_write allocates a response
buffer before calling the application. Allocation failure can bypass that
callback. This is a diagnostic possibility, not a confirmed cause of the
observed timeout. Compare the diagnostic firmware marker and entry/commit/result
logs with Android submission/callback logs for the same reproduction.

## Telemetry backpressure (diagnostics v3)

Telemetry stops its current five-value burst on allocation or notification
failure. After allocation it checks free blocks in the selected mbuf pool and
returns the buffer without sending if fewer than four blocks remain. This is
an advisory reserve for ATT traffic, not a strict reservation against concurrent
stack allocations. Checking the selected pool avoids masking its exhaustion
with free blocks from another pool. There is no retry loop and telemetry is
sampled again on the next existing periodic cycle. Failures log under ble-metrics.

This addresses unchecked telemetry pressure; hardware reproduction is still
required to establish whether it caused the observed configuration write timeout.
The ESP-IDF throughput example also gates sending on available mbufs:
[bleprph_throughput](https://github.com/espressif/esp-idf/blob/v5.4.4/examples/bluetooth/nimble/throughput_app/bleprph_throughput/main/main.c).
The boot marker is now `Config diagnostics v3: telemetry reserve=4`.

## Bound telemetry to the periodic cycle

The v3 hardware log confirmed a successful config commit while telemetry was
being deferred dozens of times per second. The runtime called load_metrics_publish
both after every FSM event (including frequent deadline checks) and in its 1 Hz
periodic cycle. This generated repeated five-notification bursts unrelated to
new measurements. Removed the per-event call; snapshots still update for every
event, and reads remain immediate. Notifications now follow only the existing
one-second scheduler. Expected buffer-pressure deferral logs at debug level;
allocation and transmission failures remain warnings. No new task or timer.
Hardware validation remains necessary for sustained telemetry and config saves
in WORKING. The BLE initialization marker is `Config diagnostics: telemetry 1Hz`.
