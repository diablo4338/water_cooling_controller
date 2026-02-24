#ifndef ECDH_H
#define ECDH_H

#include <stdint.h>

void ecdh_init(void);
int ecdh_make_dev_keys(void);
int ecdh_compute_shared_secret(const uint8_t host_pub[65], uint8_t out_secret32[32]);

#endif
