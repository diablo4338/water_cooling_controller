#ifndef OVERHEAT_ALARM_H
#define OVERHEAT_ALARM_H

#include <stdbool.h>

typedef struct {
    bool silenced;
} overheat_alarm_state_t;

void overheat_alarm_init(void);
void overheat_alarm_task(void *param);
void overheat_alarm_silence(void);

/* Pure state transition used by the task and unit tests. */
bool overheat_alarm_update(overheat_alarm_state_t *state,
                           bool alarm_enabled,
                           bool overheat_active,
                           bool silence_requested);

#endif
