#ifndef ACTION_EXECUTOR_H
#define ACTION_EXECUTOR_H

#include <stdbool.h>
#include <stddef.h>

#include "runtime_commands.h"

bool action_executor_init(void);
bool action_executor_submit(const runtime_command_t *command);
size_t action_executor_pending(void);

#endif
