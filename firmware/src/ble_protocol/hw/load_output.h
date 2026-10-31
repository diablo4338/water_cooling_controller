#ifndef LOAD_OUTPUT_H
#define LOAD_OUTPUT_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t load_output_restore(bool enabled);
esp_err_t load_output_set(bool enabled);
esp_err_t load_output_hold(void);

#endif
