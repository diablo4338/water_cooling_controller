#ifndef CONNECT_WINDOW_H
#define CONNECT_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#define CONNECT_WINDOW_US (60LL * 1000000)

typedef struct {
    int64_t deadline_us;
} connect_window_t;

static inline void connect_window_open(connect_window_t *window, int64_t now_us) {
    window->deadline_us = now_us + CONNECT_WINDOW_US;
}

static inline int32_t connect_window_remaining_ms(const connect_window_t *window,
                                                  int64_t now_us) {
    int64_t remaining = window->deadline_us - now_us;
    return remaining > 0 ? (int32_t)((remaining + 999) / 1000) : 0;
}

static inline bool connect_window_blocks_sleep(const connect_window_t *window,
                                               int64_t now_us, bool connected) {
    return connected || now_us < window->deadline_us;
}

#endif
