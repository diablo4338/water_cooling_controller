#ifndef GAP_H
#define GAP_H

void start_advertising(void);
void stop_advertising(void);
void ble_app_on_sync(void);

void term_cb(void *arg);
void data_timer_cb(void *arg);

#endif
