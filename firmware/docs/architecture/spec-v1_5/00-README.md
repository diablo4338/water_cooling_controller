# ESP32-C3 firmware architecture v1.4 — DRAFT

This is **TO-BE architecture**, not a declaration that existing firmware meets it. Source reference: `firmware(5).zip`. No code changes authorized by this package. All documents in this directory have equal authority; this README resolves conflicts with any earlier `spec-v1` documents. **Do not keep old v1.2 files beside v1.3 as competing requirements.**

## Primary objective
Operate the lamp with minimal awake time: wake → sample → update load → Deep Sleep. The lamp may remain electrically ON during Deep Sleep. BLE is initialized only after a qualified thermal-button press, once per wake cycle, and is left initialized until Deep Sleep. No routine BT host/controller shutdown/deinit. Three FSMs: Load, BLE lifecycle, Buzzer. Events are useful for separation but are **not a hard dependency for progress**: each pending phase has a deadline fallback.

## Non-negotiable sleep policy
- With no BLE activation: finish current bounded sample/operation and sleep for configured interval (currently 5 s).
- Advertising session: sleep when 60 s window expires without a connection, or after earlier session termination.
- Connected session: sleep immediately on disconnect, or at 600 s from confirmed connection regardless of whether the client is still connected. **Do not attempt BLE disconnect on 600 s expiry, do not wait for controller stop, host stop, GAP callback or client acknowledgment.** Deep Sleep terminates the radio session.
- These BLE rules are *eligibility* for sleep: if a genuinely required load/buzzer physical operation is in progress, finish or enforce its bounded fallback first. BLE may be initialized when entering Deep Sleep.
- Missing events, full queues, and missing buzzer DONE never indefinitely block sleep. No global sticky `delivery_fault` sleep veto.
- A physically failed LOAD_OFF remains an explicit hardware fault; do not falsely claim output OFF. It may keep system awake per the agreed hardware-failure exception, distinct from event-delivery failures.

## Preservation
Keep thermal-button algorithm, hardware GPIO mapping, battery thresholds, buzzer configuration, current policy, INA driver budgets, pairing/security/GATT, physical GPIO19 behavior and RTC output hold unless this document explicitly changes them. Do not change these to make tests pass.

## Source of truth
The user-approved decisions in these documents override all earlier architecture versions. Read `12-baseline-differences.md` before modifying code; never mistake target states for existing ones. Test IDs in `10-test-matrix.md` are acceptance obligations.

## Formal coverage review
Read `14-transition-coverage.md` before implementation. It defines precedence, dropped notifications, operation terminal behavior and the distinction between BLE session readiness and actual readiness to sleep. It does not require extra tasks, retries or controller stop logic.


## Implementation traceability (v1.5 addition)

See `15-implementation-map.md` for the READ-ONLY, source-verified mapping from v1.4 target behavior to firmware(5) functions, physical calls, test ideas, and explicitly identified gaps. This document does not supersede the normative behavioral specification.
