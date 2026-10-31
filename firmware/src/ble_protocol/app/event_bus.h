#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_events.h"

#define EVENT_BUS_DEPTH 64U
#define EVENT_BUS_PUBLISH_WAIT_MS 10U

bool event_bus_init(void);
bool event_bus_publish(const runtime_event_t *event);
bool event_bus_publish_isr(const runtime_event_t *event);
bool event_bus_receive(runtime_event_t *event, uint32_t timeout_ms);
size_t event_bus_pending(void);
uint32_t event_bus_overflow_count(void);

/* Serializes dispatcher work with the final sleep gate.  The dispatcher holds
 * this lock while mutating FSM state or submitting commands. */
bool event_bus_lock_dispatch(uint32_t timeout_ms);
void event_bus_unlock_dispatch(void);

/* Serializes the final sleep gate with event publication.  Once armed, new
 * publications are rejected and request that sleep be aborted. */
bool event_bus_begin_sleep_gate(void);
void event_bus_cancel_sleep_gate(void);
bool event_bus_sleep_abort_requested(void);

#endif
