#include "unity.h"

#include <string.h>

#include "state.h"
#include "storage.h"

static void fill32(uint8_t out[32], uint8_t value) {
    for (size_t i = 0; i < 32; i++) {
        out[i] = value;
    }
}

static void reset_store(void) {
    fsm_reset();
    trust_reset();
}

TEST_CASE("trust store keeps up to five clients in a ring", "[storage]") {
    reset_store();

    for (uint8_t i = 0; i < TRUST_MAX_CLIENTS; i++) {
        uint8_t host_id_hash[32];
        uint8_t key[32];
        fill32(host_id_hash, (uint8_t)(i + 1));
        fill32(key, (uint8_t)(0xA0 + i));
        trust_store_upsert(host_id_hash, key);
    }

    TEST_ASSERT_EQUAL(TRUST_MAX_CLIENTS, trust_store_count());
    TEST_ASSERT_EQUAL(0, trust_store_next_slot());

    uint8_t host6[32];
    uint8_t key6[32];
    uint8_t got_host[32];
    uint8_t got_key[32];
    fill32(host6, 6);
    fill32(key6, 0xA6);
    trust_store_upsert(host6, key6);

    TEST_ASSERT_EQUAL(TRUST_MAX_CLIENTS, trust_store_count());
    TEST_ASSERT_EQUAL(1, trust_store_next_slot());
    TEST_ASSERT_TRUE(trust_store_get_entry(0, got_host, got_key));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(host6, got_host, sizeof(got_host));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(key6, got_key, sizeof(got_key));
}

TEST_CASE("trust store updates existing client in place", "[storage]") {
    reset_store();

    uint8_t host_id_hash[32];
    uint8_t key1[32];
    uint8_t key2[32];
    uint8_t got_host[32];
    uint8_t got_key[32];
    fill32(host_id_hash, 0x11);
    fill32(key1, 0x21);
    fill32(key2, 0x31);

    trust_store_upsert(host_id_hash, key1);
    TEST_ASSERT_EQUAL(1, trust_store_count());
    TEST_ASSERT_EQUAL(1, trust_store_next_slot());

    trust_store_upsert(host_id_hash, key2);
    TEST_ASSERT_EQUAL(1, trust_store_count());
    TEST_ASSERT_EQUAL(1, trust_store_next_slot());
    TEST_ASSERT_TRUE(trust_store_get_entry(0, got_host, got_key));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(host_id_hash, got_host, sizeof(got_host));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(key2, got_key, sizeof(got_key));
}
