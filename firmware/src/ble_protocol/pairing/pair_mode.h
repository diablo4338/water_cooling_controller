#ifndef PAIR_MODE_H
#define PAIR_MODE_H

#include <stdbool.h>
#include <stdint.h>

void pair_mode_init(void);
bool pair_mode_is_active(void);
bool pair_mode_activate(void);
void pair_mode_deactivate(void);
bool pair_mode_prepare_session(void);
/* Millisecond deadline token for the current pairing window.  A queued timeout
 * event carries this value so it cannot end a newer pairing window. */
uint32_t pair_mode_deadline_ms(void);

#endif
