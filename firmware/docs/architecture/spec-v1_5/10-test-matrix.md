# Acceptance tests — v1.4

Each automated test should exercise actual production reducers and deadlines. For each test record outcome and source path; hardware-specific results remain unverified until real-device testing.

| ID | Scenario | Expected |
|---|---|---|
| T01 | No button wake | No BLE initialization; return to sleep |
| T02 | Retained load WORKING | GPIO restore ON; may sleep; no startup sound replay |
| T03 | Invalid RTC data | Default load OFF |
| T04 | Button pressed IDLE | Real LOAD_ON before settle begins |
| T05 | Settle timer notification lost | Time check starts fresh INA request at >=1000ms |
| T06 | INA STARTUP success normal battery | Normal configured startup signal once |
| T07 | INA STARTUP success low battery | Configured low battery/startup signal once |
| T08 | INA STARTUP failure | Exactly three short beeps once |
| T09 | INA periodic failure | No error beeps; low-current count reset |
| T10 | INA shutdown failure | Silent, proceed LOAD_OFF |
| T11 | Shutdown battery normal | No shutdown sound; LOAD_OFF |
| T12 | Shutdown low battery | Beep before LOAD_OFF |
| T13 | Buzzer DONE dropped | Playback deadline forces sound off, LOAD_OFF proceeds |
| T14 | Repeated/stale buzzer DONE | No duplicate load toggle |
| T15 | Load GPIO OFF actually fails | Do not claim IDLE; physical-fault handling |
| T16 | Load WORKING while no BLE | May Deep Sleep |
| T17 | BLE without button on boot | NimBLE init not called |
| T18 | First button | BLE init exactly once |
| T19 | Second button during STARTING | No extra init; window deadline refreshed |
| T20 | Second button during ADVERTISING | Window deadline refreshed |
| T21 | 60-s advertising window expires | Direct Deep Sleep eligibility without stop ack |
| T22 | BLE client connects | Record connection timestamp and 600-s hard deadline |
| T23 | Client disconnects early | Deep Sleep; never restart advertising |
| T24 | Client remains connected 600 s | Direct Deep Sleep, no disconnect attempt |
| T25 | Connection traffic at 599 s | Does not extend 600-s deadline |
| T26 | Client press while CONNECTED | Does not extend 600-s deadline |
| T27 | Press in READY_TO_SLEEP | No re-init this awake cycle |
| T28 | BLE start fails/hangs | Bounded fallback to sleep (not sticky ERROR) |
| T29 | Full event queue | No permanent sleep veto; deadlines still progress |
| T30 | Missed BLE callback | Deadline still triggers sleep |
| T31 | Missing timer callback | Poll deadline drives state transition |
| T32 | Startup op and late INA result | No duplicate beeps |
| T33 | Shutdown op and late buzzer completion | No output reactivation |
| T34 | Load startup still settling at BLE deadline | Finish/bound startup then sleep |
| T35 | Active BLE client at normal load sample tick | Stay awake until disconnect or 600s |
| T36 | Physical GPIO ON failure | No fake ON confirmation |
| T37 | Deep Sleep resets BLE runtime | Next wake BLE OFF |
| T38 | Existing pairing and auth flows | No changes to security/GATT behavior |
| T39 | Existing GPIO19 button | Retains pairing/reset behavior |
| T40 | Existing INA driver | Configuration and internal timeouts unchanged |

## Additional boundary acceptance tests
| ID | Scenario | Expected |
|---|---|---|
| T41 | At deadline, both timeout and success completion observed | Exactly one terminal transition/action |
| T42 | BLE CONNECT arrives concurrently with advertising window deadline | Connection is accepted only if validated before window closure; otherwise sleep; deterministic order recorded |
| T43 | BLE disconnect event arrives after connection hard deadline | Sleep once, no advertising restart |
| T44 | Buzzer DONE lost during startup | Startup deadline allows WORKING without second sound |
| T45 | New periodic current sample and shutdown start coincide | Shutdown is not undone by late periodic result |
| T46 | Thermal press while load WORKING and BLE READY_TO_SLEEP | Load unchanged, no BLE reinit; allow sleep after other bounded work |
| T47 | Failed queue publish for shutdown DONE | Shutdown finishes at local deadline; buzzer pin forced OFF before LOAD_OFF |
| T48 | Invalid retained RTC after cold boot | Default load OFF; no fake startup beep |
| T49 | First BLE button press during startup load settle | BLE and load sequences independent; BLE deadline does not truncate required settle |
| T50 | BLE READY arrives after startup timeout | Ignore stale ready, never begin advertising |
| T51 | Timers wrap or deadline arithmetic overflow | Deadline comparison is wrap-safe for supported monotonic time representation |
| T52 | Pending hardware action at nominal Deep Sleep decision | Allow only finite required action completion/fallback; no missing-event indefinite wait |
