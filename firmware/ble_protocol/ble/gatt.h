#ifndef GATT_H
#define GATT_H

#include "host/ble_gatt.h"

void gatt_init_uuids_and_services(void);

extern struct ble_gatt_svc_def gatt_svcs[];

#endif
