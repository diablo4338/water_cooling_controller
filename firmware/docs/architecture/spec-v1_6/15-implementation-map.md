# Implementation mapping — target architecture v1.4 vs firmware(5)

**Status:** READ-ONLY source review. Paths below are relative to `firmware/`; line numbers refer to the provided `firmware(5).zip`. **AS-IS** describes actual source; **TO-BE** describes v1.4, not yet implemented. Nothing in this document authorizes an agent to treat proposed behavior as present.

## Existing execution topology

- `src/ble_protocol/app/main.c:51` (`app_main`) initializes configuration, retained load state, sensors, legacy pairing and a runtime task. NimBLE is intentionally lazy; do not remove the lazy-start property.
- `src/ble_protocol/app/load_runtime.c:276` (`process_event_locked`) is the real integration point. It passes one event to `src/ble_protocol/control/runtime_core.c:28` (`runtime_core_step`) and processes generated commands through `apply_commands_locked` (`load_runtime.c:220`). This calls the three `*_fsm_step` functions sequentially; the FSMs are not independently subscribed FreeRTOS tasks.
- `src/ble_protocol/app/action_executor.c:409` (`action_executor_task`) executes commands and publishes results. `src/ble_protocol/app/event_bus.c` implements the existing queue, overflow staging and sleep gate.
- `src/ble_protocol/app/load_runtime.c:480` (`scheduler_locked`) checks periodic samples, deadlines and sleep; `maybe_request_sleep_locked` (`:453`) submits the sleep command; `src/ble_protocol/app/action_executor.c:285` (`execute_sleep`) uses power gate; `load_runtime_prepare_sleep` (`load_runtime.c:644`) enters Deep Sleep.
- Pairing/auth state machines in `src/ble_protocol/pairing/` are separate from BLE lifecycle; preserve their protocol and cryptographic behavior.

## Traceability: transitions, effects and code changes

| ID | Target scenario / event | Existing state handler / source | Existing physical execution | Needed change to meet v1.4 | Suggested verification |
|---|---|---|---|---|---|
| MAP-01 | Wake, no button -> periodic measurement -> Deep Sleep | `main.c:51`; `load_runtime.c:400,480`; `runtime_core.c:28` | `action_executor.c:225` calls `ina226_get_single_sample_ex`; `:285` sleep | Preserve lazy BLE; ensure bounded periodic work and no global event fault veto | Host integration: zero BLE start commands; sleep command eventually submitted |
| MAP-02 | Thermal press -> Load `IDLE` to `STARTING` | `load_fsm.c:70` `load_fsm_step`; `load_runtime.c:276` routes | `action_executor.c:210` `execute_load_output` uses `load_output_set` and emits GPIO applied/failed | Keep physical acknowledgment before any settle work | Command ordering and `RTEVT_LOAD_GPIO_APPLIED` correlation test |
| MAP-03 | GPIO ON -> 1000-ms settle -> fresh voltage | `buzzer_fsm.c:241` `buzzer_fsm_step` | `action_executor.c:325` timer actions; `:225` INA request | Preserve deadline fallback if settle callback missing; do not read stale periodic voltage | Inject missing timer callback; verify INA requested only after 1000 ms |
| MAP-04 | STARTUP INA failure -> exactly one triple beep | `buzzer_fsm.c:241` and `:182` | `action_executor.c:167` `beep_start`, `:118` timer playback | Do not emit triple pattern on periodic INA failures; fallback finishes if signal callback missing | Pattern pulse count and duplicate/stale measurement tests |
| MAP-05 | WORKING low-current timer -> `STOPPING` | `load_fsm.c:70` | `runtime_core.c:40` maps `RCMD_BUZZER_REQUEST_SHUTDOWN` into event | Preserve real output ON until shutdown buzzer ends/deadline | Assert `LOAD_OFF` absent before bounded completion |
| MAP-06 | SHUTDOWN INA result -> beep only if battery low | `buzzer_fsm.c:241` | `action_executor.c:225` INA and `:167` buzzer | Keep configurable threshold; errors/normal voltage silent; missing DONE cannot stall LOAD_OFF | Threshold boundary; INA error; missing DONE deadline |
| MAP-07 | `STOPPING` -> actual GPIO OFF -> `IDLE` | `load_fsm.c:70` and `:139` | `action_executor.c:210`; returns `RTEVT_LOAD_GPIO_APPLIED/FAILED` | Never mark physical OFF without successful GPIO execution; hardware output failure policy remains distinct from lost event | Inject GPIO-off failure; no false `IDLE` |
| MAP-08 | Thermal press -> BLE lazy init | `ble_fsm.c:107`; `load_runtime.c:276` | `action_executor.c:246` `RCMD_BLE_START` -> `gap_ble_start` in `ble/gap.c:303` | Retain one start per wake; no re-init after session finished | Count `gap_ble_start` calls under duplicate presses |
| MAP-09 | BLE ready -> 60-s advertising | `ble_fsm.c:107`; GAP callback path | `action_executor.c:261` -> `gap_start_advertising` (`gap.c:248`) | Maintain finite deadline and extension on press only while eligible | 60-s timeout; button renewal in ADVERTISING |
| MAP-10 | Client connected -> disconnect or 600-s deadline -> sleep | `ble_fsm.c:107` (current code has no 600-s connected deadline) | GAP callback via `gap_event`; sleep request currently requires OFF | **Replace STOPPING flow with READY_TO_SLEEP.** 600-s deadline starts at accepted connection. Do not manually disconnect or stop controller | Disconnect at any point; deadline without callback -> Deep Sleep command |
| MAP-11 | Advertising deadline -> sleep | `ble_fsm.c:246` currently calls `begin_stop` | `RCMD_BLE_STOP` -> `gap_ble_stop` (`gap.c:370`) | **Remove manual BLE Host/controller stop**; transition to READY_TO_SLEEP, sleep directly after remaining bounded work | No `RCMD_BLE_STOP`, no controller deinit call |
| MAP-12 | Pairing and auth compatibility | Pair-mode path in `load_runtime.c:355`, GAP code `gap.c` | `RCMD_BLE_DISCONNECT` and pair controls exist | Audit button GPIO19 semantics, timeouts and paired/unpaired handling; do not confuse pairing timeout with lifecycle deadline | Pairing/auth regression tests; ensure no unauthorized re-advertisement |
| MAP-13 | Wake RTC valid/invalid -> restore output | `load_runtime.c:145` `retained_init` | `load_output_set` / hold setup in runtime and output driver | Valid RTC restore requested ON/OFF; invalid RTC -> OFF. No redundant verification protocol | Cold boot and Deep Sleep wake state tests |
| MAP-14 | Power decision independent of queue failures | `power_manager.c` and `action_executor.c:285`; `event_bus.c` | Existing `delivery_fault`, queued commands and sleep gate | **Remove permanent sleep veto for event delivery failure**; bounded FSM deadlines remain decisive. Preserve truth of physical GPIO failures | Overflow/stale completion: deadline still reaches sleep |
| MAP-15 | Deadline checks under continuous events | `load_runtime.c:480` `scheduler_locked` | `runtime_core_step` dispatches `RTEVT_DEADLINE_CHECK` | Guarantee bounded event processing then check all deadlines and sleep | Flood queue and assert timer progress |

## Direct conflicts with v1.4 (verified)

1. `control/ble_fsm.c:57-83, 223-296` still implements `STOPPING`, `RCMD_BLE_STOP`, retries, `pending_restart` and reconnect-related logic; v1.4 requires `READY_TO_SLEEP` and **no manual controller stop or re-init this wake**.
2. `control/ble_fsm.c:246` has deadline checks for STARTING / ADVERTISING only, not a 600-second connected-session deadline.
3. `control/power_manager.c` only permits BLE `OFF` and checks `ble_radio_quiet`, whereas v1.4 must allow sleep in logical `READY_TO_SLEEP` even if Host/controller remains initialized.
4. `app/action_executor.c:246-281` still dispatches stop/disconnect commands to GAP; v1.4 does not need these for lifecycle expiry. **Do not blindly remove** operations still used for pairing security; map each caller before editing.
5. `control/load_fsm.c:70` handles `RTEVT_EVENT_BUS_FAULT` by moving to `LOAD_FSM_ERROR`; `control/ble_fsm.c:107` does similarly; `app/event_bus.c` tracks `delivery_fault`; `app/action_executor.c:285` uses `event_bus_sleep_abort_requested`. v1.4 disallows **unbounded sleep prohibition due merely to lost events**. Distinguish temporary sleep coordination from permanent sticky faults.
6. `app/load_runtime.c:453` short-circuits on `event_bus_pending() != 0`; ensure bounded event processing so that busy event input does not indefinitely starve deadlines/sleep.

## Keep / replace boundaries

**KEEP:** physical GPIO implementation, INA226 driver and existing API budgets, configuration values, RTC GPIO hold, pairing/authentication protocol, GATT identities, normal sensor reading, button detection, existing timeout values unless specifically changed by v1.4.

**MODIFY:** BLE lifecycle transitions and 600-s client deadline; power manager BLE sleep eligibility; stale/duplicate event behavior if needed; overflow/failed-callback fallback where sticky error prevents the intended timed sleep; tests for behavior above.

**REMOVE as lifecycle requirements:** `BLE_STOPPING`, `DISCONNECTING`, controller shutdown acknowledgment, BLE stop retries, repeated lifecycle starts in one wake, global delivery-fault-based permanent sleep veto. Do not alter pairing-specific use of disconnect without checking security implications.

## Verification plan

1. Run existing host tests **before editing**; record baseline.
2. Add RED tests for MAP-10/11/14/15 that assert target v1.4 behavior on production FSM and power manager code.
3. Make targeted BLE FSM + Power Manager changes; independently review GAP/pairing changes, if any.
4. Run all host tests; build with ESP-IDF if available. Report unavailable verifications honestly.
5. Only then verify startup/shutdown and RTC paths under board testing; document actual current waveform and wake timing separately.

## Important open technical facts (not product decisions)

- Whether the current periodic loop's configured scheduling under sustained queue pressure satisfies MAP-15 requires a targeted integration test.
- The exact events and command owners for pairing-specific disconnect must be traced before removing any GAP API calls.
- Host tests alone cannot prove radio current or the Deep Sleep wake circuit on hardware.
