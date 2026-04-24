#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#define TRUST_MAX_CLIENTS 5

void nvs_load_or_empty(void);
bool trust_store_match_auth(const uint8_t auth_nonce[16],
                            const uint8_t proof[32],
                            uint8_t out_host_id_hash[32]);
void trust_store_upsert(const uint8_t host_id_hash[32], const uint8_t k[32]);
uint8_t trust_store_count(void);
uint8_t trust_store_next_slot(void);
bool trust_store_get_entry(uint8_t slot, uint8_t out_host_id_hash[32], uint8_t out_k[32]);
void trust_reset(void);

#endif
