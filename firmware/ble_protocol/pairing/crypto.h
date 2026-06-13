#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <stdint.h>

void rand_bytes(uint8_t *out, size_t n);
int my_rng(void *ctx, unsigned char *out, size_t len);

int pair_hmac_sha256(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     uint8_t out[32]);

int hkdf_sha256_32(const uint8_t *salt, size_t salt_len,
                   const uint8_t *ikm, size_t ikm_len,
                   const uint8_t *info, size_t info_len,
                   uint8_t out32[32]);

int sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32]);

#endif
