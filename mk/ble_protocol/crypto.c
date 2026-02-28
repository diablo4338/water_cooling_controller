#include <string.h>

#include "esp_random.h"

#include "mbedtls/md.h"

#include "crypto.h"

void rand_bytes(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        uint32_t r = esp_random();
        size_t chunk = (n - i < 4) ? (n - i) : 4;
        memcpy(out + i, &r, chunk);
    }
}

int my_rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    rand_bytes(out, len);
    return 0;
}

int hmac_sha256(const uint8_t *key, size_t key_len,
                const uint8_t *msg, size_t msg_len,
                uint8_t out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    if (mbedtls_md_hmac(md, key, key_len, msg, msg_len, out) != 0) return -1;
    return 0;
}

int hkdf_sha256_32(const uint8_t *salt, size_t salt_len,
                   const uint8_t *ikm, size_t ikm_len,
                   const uint8_t *info, size_t info_len,
                   uint8_t out32[32]) {
    uint8_t prk[32];
    if (hmac_sha256(salt, salt_len, ikm, ikm_len, prk) != 0) return -1;

    uint8_t buf[128];
    if (info_len + 1 > sizeof(buf)) return -1;
    memcpy(buf, info, info_len);
    buf[info_len] = 0x01;

    if (hmac_sha256(prk, sizeof(prk), buf, info_len + 1, out32) != 0) return -1;
    return 0;
}

int sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, 0) != 0) { mbedtls_md_free(&ctx); return -1; }
    if (mbedtls_md_starts(&ctx) != 0) { mbedtls_md_free(&ctx); return -1; }
    if (mbedtls_md_update(&ctx, msg, msg_len) != 0) { mbedtls_md_free(&ctx); return -1; }
    if (mbedtls_md_finish(&ctx, out) != 0) { mbedtls_md_free(&ctx); return -1; }
    mbedtls_md_free(&ctx);
    return 0;
}
