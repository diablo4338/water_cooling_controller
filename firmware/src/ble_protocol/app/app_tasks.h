#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <stdbool.h>

void button_task(void *p);
void host_task(void *param);
void app_tasks_init(void);
bool app_factory_reset_recover(void);

#endif
