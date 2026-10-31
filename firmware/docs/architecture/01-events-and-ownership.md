# 01 — Events, ownership, sequencing

## Components

```
ADS1115 + thermal-edge detector  ─┐
INA226 sample adapter             ├──> SERIAL EVENT DISPATCHER ──> Load FSM ──┐
Timer/deadline scheduler          ┤                         ├──> BLE FSM ───┤
NimBLE/GAP callbacks              ┤                         └──> Buzzer FSM ┤
Action completion callbacks       ┘                                         │
                                                               ACTION EXECUTOR
                                                                GPIO/INA/BLE/
                                                               timers/buzzer
                                                                         │
                                                               POWER MANAGER
                                                               Deep Sleep gate
```

FSM steps are deterministic state + event -> next state + *commands*. Hardware callbacks and timers **publish events only**, do not call other FSMs or directly enter Deep Sleep. All events/state mutations serialized in one worker context. Use a bounded queue or equivalent serializing mechanism for asynchronous producers; non-blocking ISR publication. Ordering across simultaneous producers must not be relied on for correctness.

## Event envelope

Suggested C contracts (illustrative, not drop-in SDK code):

```c
typedef enum {
  EVT_THERMAL_BUTTON_PRESSED,
  EVT_CURRENT_SAMPLE_OK, EVT_INA_SAMPLE_FAILED,
  EVT_VOLTAGE_SAMPLE_OK,
  EVT_LOAD_GPIO_APPLIED, EVT_LOAD_GPIO_FAILED,
  EVT_SETTLE_ELAPSED, EVT_BEEP_FINISHED,
  EVT_WINDOW_EXPIRED, EVT_BLE_READY, EVT_BLE_START_FAILED,
  EVT_CLIENT_CONNECTED, EVT_CLIENT_DISCONNECTED,
  EVT_ADV_COMPLETE, EVT_BLE_STOPPED, EVT_BLE_STOP_FAILED,
  EVT_DEADLINE_CHECK
} event_kind_t;
typedef enum { MEASURE_PERIODIC, MEASURE_STARTUP, MEASURE_SHUTDOWN } measure_context_t;
typedef struct {
  event_kind_t kind;
  uint32_t operation_id;  // identify owning operation; 0 for unsolicited events
  measure_context_t context;
  int64_t timestamp_us;
  // tagged payload: current_ma, voltage_v, error code, conn_handle etc.
} event_t;
```

A combined INA226 sample can be represented as one `INA_SAMPLE_OK` with both values rather than two events. `EVT_INA_SAMPLE_FAILED` must be sent once per failed *request*, with its `operation_id` and context. Do not confuse raw driver call results with a command to beep. Physical output changes must be acknowledged (`EVT_LOAD_GPIO_APPLIED`) before a settle deadline starts.

## Producers and consumers

| Producer | Event | Consumers | Required semantics |
|---|---|---|---|
| Thermal detector (ADS1115) | `THERMAL_BUTTON_PRESSED` | Load, BLE | One rising edge; skip invalid ADS readings; no implicit repeat |
| INA226 adapter | `INA_SAMPLE_OK` | Load for current; Buzzer for requested voltage | Correlated request id/context; only valid finite values |
| INA226 adapter | `INA_SAMPLE_FAILED` | Load and Buzzer | Load resets low-current pending; Buzzer decides from operation context |
| Load GPIO executor | `LOAD_GPIO_APPLIED/FAILED` | Load, Buzzer (via dispatcher) | Notify after hardware action; failed ON must not trigger startup success |
| Timer scheduler | `SETTLE_ELAPSED`, `WINDOW_EXPIRED`, `DEADLINE_CHECK` | Buzzer, BLE, Load | Check current deadline/token when delivered; discard stale timer events |
| NimBLE host sync | `BLE_READY` | BLE | Only accepted in matching STARTING generation |
| NimBLE GAP | `CLIENT_CONNECTED`, `CLIENT_DISCONNECTED`, `ADV_COMPLETE` | BLE | Capture and validate session handle/generation |
| BLE adapter | `BLE_STOPPED/FAILED` | BLE | Confirm host/controller truly quiescent before OFF |
| Buzzer executor | `BEEP_FINISHED` | Buzzer | One completion per pattern ID |
| Buzzer FSM | `BUZZER_DONE(reason, operation_id)` | Load | `SHUTDOWN` completion unblocks physical load OFF; `STARTUP` merely completes the cue |

## Critical rules
- `REQUEST_ID`: unique within uptime; late/stale voltage results must never change another operation or cause a beep.
- Give every timeout a generation/token; a stale window timeout must not defeat a new button press.
- Button events while BLE STARTING extend its deadline; ADVERTISING extends deadline; CONNECTED ignores for BLE; STOPPING remembers request for restart after stop. Load WORKING ignores new presses.
- Never block dispatcher for the one-second load settling, audio playback, INA conversion, or BLE shutdown. Completion events are the synchronization point.
- `gettimeofday()` is wall-clock, **not monotonic**. Specify a monotonic-across-deep-sleep source or robust elapsed-time strategy for load low-current timing; `esp_timer_get_time()` alone resets after deep sleep. If preserving existing wall-clock approach, test time discontinuities and record its limitation.
- Runtime snapshot used by GATT reflects confirmed physical output and fresh measurement, not a prematurely changed FSM mode.
