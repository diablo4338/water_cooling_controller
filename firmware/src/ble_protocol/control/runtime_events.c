#include "runtime_events.h"

static uint32_t s_next_id = 1U;

uint32_t runtime_event_next_id(void) {
    uint32_t id = s_next_id++;
    if (s_next_id == 0U) s_next_id = 1U;
    if (id == 0U) id = s_next_id++;
    return id;
}

void runtime_event_reset_ids(uint32_t next_id) {
    s_next_id = next_id ? next_id : 1U;
}
