#ifndef NVS_H
#define NVS_H

#include <stddef.h>

#include "esp_err.h"

typedef int nvs_handle_t;

#define NVS_READONLY 0
#define NVS_READWRITE 1

esp_err_t nvs_open(const char *namespace_name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

#endif
