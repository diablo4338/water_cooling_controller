#ifndef PAIR_MODE_H
#define PAIR_MODE_H

#include <stdbool.h>

typedef enum {
    PAIR_WINDOW_OPEN_REQUEST,
    PAIR_WINDOW_EXPIRED,
    PAIR_WINDOW_PAIR_COMPLETED,
    PAIR_WINDOW_LINK_CLOSED,
    PAIR_WINDOW_FACTORY_RESET,
} pair_window_event_t;

void pair_mode_init(void);
bool pair_mode_is_active(void);
void pair_mode_request_open(void);

// Call from the NimBLE host task.
void pair_mode_dispatch(pair_window_event_t event);

#endif
