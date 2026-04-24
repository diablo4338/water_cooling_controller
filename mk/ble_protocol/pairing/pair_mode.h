#ifndef PAIR_MODE_H
#define PAIR_MODE_H

#include <stdbool.h>

void pair_mode_init(void);
bool pair_mode_is_active(void);
bool pair_mode_activate(void);
void pair_mode_deactivate(void);
bool pair_mode_prepare_session(void);

#endif
