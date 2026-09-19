#ifndef LOAD_OUTPUT_H
#define LOAD_OUTPUT_H

#include <stdbool.h>

void load_output_restore(bool enabled);
void load_output_set(bool enabled);
void load_output_hold(void);

#endif
