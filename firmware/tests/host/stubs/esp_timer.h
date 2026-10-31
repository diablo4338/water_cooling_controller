#ifndef HOST_ESP_TIMER_H
#define HOST_ESP_TIMER_H

#include <stdint.h>

#include "esp_err.h"

typedef void (*esp_timer_cb_t)(void *arg);
typedef struct host_timer *esp_timer_handle_t;

typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    int dispatch_method;
    const char *name;
} esp_timer_create_args_t;

#define ESP_TIMER_TASK 0

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);

#endif
