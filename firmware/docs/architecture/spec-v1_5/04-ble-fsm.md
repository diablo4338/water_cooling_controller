# BLE lifecycle — simplified, time-driven

**Never initialize BLE at ordinary boot.** On first qualified thermal-button/pairing activation initialize NimBLE once per awake cycle. Leave stack/controller running until Deep Sleep. No controller stop/deinit, no NimBLE host shutdown/deinit, no repeated initialization in same wake. Existing pairing and auth FSMs remain unchanged.

States: `OFF`, `STARTING`, `ADVERTISING`, `CONNECTED`, `READY_TO_SLEEP`. Optional `ERROR` represents startup failure and is handled by bounded sleep fallback, **not** an indefinite awake latch.

| State | Trigger | Next | Action |
|---|---|---|---|
| OFF | authorized thermal press | STARTING | Initialize once; set advertising deadline now + 60 s |
| STARTING | repeat press | STARTING | Only refresh window deadline; no re-init |
| STARTING | real host ready, window valid | ADVERTISING | Start advertising |
| STARTING | startup error or bounded startup timeout | READY_TO_SLEEP | Log error; normal bounded sleep path |
| ADVERTISING | repeat press | ADVERTISING | Extend window deadline 60 s |
| ADVERTISING | client connects | CONNECTED | Set **hard connection deadline**: connection time + 600 s |
| ADVERTISING | no client and window deadline reached | READY_TO_SLEEP | May request advertising stop, but do not await confirmation before sleep |
| CONNECTED | thermal press | CONNECTED | Ignore for BLE |
| CONNECTED | client disconnects | READY_TO_SLEEP | Do not advertise again |
| CONNECTED | now >= connection deadline | READY_TO_SLEEP | **Go to Deep Sleep directly. Do not disconnect first.** |
| READY_TO_SLEEP | press | READY_TO_SLEEP | Do not reinitialize/advertise before next wake |

**Sleep policy:** READY_TO_SLEEP permits immediate Deep Sleep once bounded mandatory load/buzzer actions finish, whether NimBLE still reports active advertising/client or not; `esp_deep_sleep_start()` ends the session. 60 s window counts from activation/latest qualifying repeat press when not connected. 600 s counts strictly from successful connection, independent of activity. If callbacks fail or get lost, deadline must still make progress; do not wait for BLE STOPPED, ADV_COMPLETE, or a disconnect ACK. No 60-s extra disconnect deadline, no two disconnect attempts, no DISCONNECTING/STOPPING state.

A real connected client may momentarily remain connected as the chip enters Deep Sleep; this is intentional. No attempt to guarantee polite protocol disconnect. Preserve GPIO19 physical pairing/reset button behavior and existing security/GATT configuration. Distinguish pairing timeout/security from this **overall** BLE lifecycle policy; old timers must not accidentally restart advertising after READY_TO_SLEEP.
