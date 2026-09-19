#ifndef GAP_H
#define GAP_H

void gap_init(void);
void start_advertising(void);
void stop_advertising(void);
void ble_app_on_sync(void);
void gap_service_poll(void);
void gap_disconnect_client(void);

void term_cb(void *arg);

#endif
