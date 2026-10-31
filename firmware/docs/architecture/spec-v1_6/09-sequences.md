# End-to-end sequences

## Ordinary wake
`WAKE → BLE OFF → restore Load from RTC (or default OFF) → temperature/current sample → Load transition/deadline check → Power decision → Deep Sleep (configured period)`. No NimBLE initialization, even when load remains ON.

## Button triggers startup and BLE
`THERMAL_BUTTON_PRESSED → Load STARTING + BLE STARTING → physical LOAD_ON → settle 1000 ms (time check, no blocking wait) → fresh INA voltage → normal/low-battery pattern OR single three-short-error pattern → sound finished OR sound deadline → Load WORKING`. BLE independently initializes once and begins advertising. Never re-init on duplicate press.

## Low-current shutdown
`low-current deadline → Load STOPPING, output still ON → fresh INA voltage → below threshold? configured low-battery sound : silence → BUZZER_DONE OR sound deadline → force buzzer pin OFF → physical LOAD_OFF → Load IDLE`. No signal for normal battery, no INA-error triple on shutdown.

## BLE no connection
`BLE STARTING → ADVERTISING → 60s deadline reached → READY_TO_SLEEP → Deep Sleep`. It is acceptable to stop advertising first when trivial, but **never wait** for stop confirmation. No radio controller teardown.

## BLE client disconnects
`CLIENT_CONNECTED → CONNECTED → CLIENT_DISCONNECTED → READY_TO_SLEEP → Deep Sleep`. Do not restart advertising. No disconnect retry or controller-stop confirmation.

## BLE connected too long
`CLIENT_CONNECTED at T → still connected at T+600s → READY_TO_SLEEP → Deep Sleep`. No call to terminate connection and no extra 60-s wait.

## Missing BUZZER_DONE
`Load STOPPING → sound started → DONE lost → local sound deadline → buzzer pin forced OFF → LOAD_OFF → sleep when eligible`. Device must not get stuck simply because an event was dropped.

## Queue overflow
`event queue full → counter/log → dispatcher continues → deadline checks continue → policy-based sleep`. No sticky delivery_fault preventing sleep.
