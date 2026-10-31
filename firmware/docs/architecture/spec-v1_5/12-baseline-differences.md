# Current `firmware(5).zip` versus target v1.3

Verify actual functions against your checkout before edits. The current code reflects prior architecture iterations and may include functionality **now explicitly rejected**:

- Remove any routine BLE STOPPING/DISCONNECTING/controller stop/deinit path; no graceful disconnect on 600-s cap. Deep Sleep ends session.
- Old 60-s BLE disconnect retry + two attempts is **superseded**; do not retain as a requirement.
- Old `delivery_fault` / sticky ISR event overflow sleep veto is **superseded**. Keep simple bounded queue/event dispatch but independent deadline progression.
- BLE-initiated idle may sleep without awaiting `ADV_COMPLETE` or radio-off confirmation.
- Keep `LOAD_WORKING` sleepable; do not use a global all-FSMs-IDLE sleep condition.
- On buzzer missing DONE timeout, force output silent and continue shutdown; do not remain in Buzzer ERROR awake forever.
- Maintain load physical failure handling and existing INA driver behavior. Preserve security pairing/auth FSMs and GATT mappings.
- RTC validity check only: restore valid state or set default OFF; do not add extra RTC/GPIO consistency-machine behavior.

The spec makes no claim which of these has already been implemented. An evidence-based source audit must establish that.
