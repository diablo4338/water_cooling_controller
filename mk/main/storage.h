#ifndef STORAGE_H
#define STORAGE_H

void nvs_load_or_empty(void);
bool nvs_load_trust_if_available(uint8_t out_host_id_hash[32], uint8_t out_k[32]);
void nvs_save_trust(void);
void trust_reset(void);

#endif
