# 03 — BLE FSM contract

States `OFF`, `STARTING`, `ADVERTISING`, `CONNECTED`, `STOPPING`, optional `ERROR` for unrecoverable adapter failures. Maintain explicit `initialized`/`controller_enabled` internal adapter state, not as independent app FSM switches.

| Current | Event | Next | Effect |
|---|---|---|---|
| OFF | THERMAL_BUTTON_PRESSED | STARTING | Initialize if first use this uptime, otherwise restart permitted host/controller; open 60 s deadline |
| STARTING | THERMAL_BUTTON_PRESSED | STARTING | Extend deadline, no second init |
| STARTING | BLE_READY | ADVERTISING | Start advertising *only if deadline still valid*, else STOPPING |
| STARTING | BLE_START_FAILED | STOPPING/ERROR | Attempt bounded cleanup; never claim OFF before complete |
| ADVERTISING | THERMAL_BUTTON_PRESSED | ADVERTISING | Extend 60 s deadline |
| ADVERTISING | CLIENT_CONNECTED | CONNECTED | Cancel/stop advertising; capture active connection |
| ADVERTISING | WINDOW_EXPIRED, validated current deadline | STOPPING | Stop advertising and host/controller safely |
| ADVERTISING | ADV_COMPLETE without connection | STOPPING | Do not restart advertising silently |
| CONNECTED | WINDOW_EXPIRED | CONNECTED | Leave client connected; do not resume advertising later |
| CONNECTED | THERMAL_BUTTON_PRESSED | CONNECTED | Ignore for BLE |
| CONNECTED | CLIENT_DISCONNECTED | **STOPPING** | **Always stop, even if 60 s not elapsed.** Never restart adv on disconnect |
| STOPPING | THERMAL_BUTTON_PRESSED | STOPPING | Record restart requested |
| STOPPING | BLE_STOPPED | OFF or STARTING | OFF if no pending press; else STARTING with fresh window |
| STOPPING | BLE_STOP_FAILED | ERROR | Bounded recovery or safe restart, no premature sleep |

The **GPIO19 physical pairing/reset button** is distinct from the thermal button and retains original global reset/pairing semantics. Verify its BLE window interaction separately; no regression of stored trust credentials or pairing protocol.

On boot after cold boot or Deep Sleep: BLE **not initialized**, FSM `OFF`. Keep NimBLE startup out of normal `app_main` path without a window. Current firmware already gates stack startup via `load_runtime_window_remaining_ms()`. Preserve that behavior while refactoring. `menuconfig` enables BLE code but does not itself turn the radio on.

No `esp_bt_controller_deinit()` in repeatable active-session stop/start, no irreversible `esp_bt_mem_release()`. Correct NimBLE host shutdown must be confirmed against installed ESP-IDF version. Radio fully quiet before claiming `OFF`; advertising stopped is not identical to stopped controller. Power Manager must not sleep while BLE is STARTING, ADVERTISING, CONNECTED or STOPPING.

Policy exception knowingly retained: connected BLE client can hold device awake beyond 60 s until disconnect. Do not silently add a hard connection deadline; report it as energy-use risk.
