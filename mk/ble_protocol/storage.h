#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>

void nvs_load_or_empty(void);
bool nvs_load_trust_if_available(uint8_t out_host_id_hash[32], uint8_t out_k[32]);
void nvs_save_trust(void);
void trust_reset(void);
bool nvs_is_pair_forced(void);
void nvs_set_pair_forced(bool enabled);

#endif
