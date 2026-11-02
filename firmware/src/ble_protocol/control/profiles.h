#ifndef PROFILES_H
#define PROFILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROFILE_COUNT 3
#define PROFILE_STATUS_VERSION 1
#define PROFILE_STATUS_PAYLOAD_LEN 2
#define PROFILE_METADATA_VERSION 1
#define PROFILE_TITLE_MAX_LEN 32
#define PROFILE_DESCRIPTION_MAX_LEN 96
#define PROFILE_METADATA_MAX_LEN (3 + PROFILE_TITLE_MAX_LEN + PROFILE_DESCRIPTION_MAX_LEN)

#define PROFILE_ACTION_SAVE_CURRENT 1
#define PROFILE_ACTION_APPLY 2
#define PROFILE_ACTION_CLEAR 3

bool profiles_read_mask(uint8_t *mask);
bool profiles_save_current(uint8_t slot);
bool profiles_apply(uint8_t slot, uint8_t *status, uint8_t *field_id);
bool profiles_clear(uint8_t slot);
bool profiles_read_metadata(uint8_t slot, uint8_t *out, size_t capacity, size_t *len);
bool profiles_write_metadata(uint8_t slot, const uint8_t *data, size_t len);

#endif
