#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "device_status.h"
#include "device_status_ble.h"
#include "metrics.h"
#include "thermal_protection.h"

static float temperature;
static bool sample_available = true;
static uint8_t last_notification[DEVICE_STATUS_PAYLOAD_LEN];

int64_t esp_timer_get_time(void) { return 0; }
metrics_adc_state_t metrics_get_adc_state(void) { return METRICS_ADC_AVAILABLE; }
bool metrics_has_ads_error(void) { return false; }
bool metrics_has_ina_error(void) { return false; }
void state_lock(void) {}
void state_unlock(void) {}
uint16_t fsm_get_auth_conn_handle(void) { return 1; }

bool metrics_get_temp_sample(uint8_t channel, float *temp, int64_t *sampled_us) {
    assert(channel == METRICS_CONTROL_TEMP_CHANNEL);
    *temp = temperature;
    *sampled_us = 0;
    return sample_available;
}

void device_status_notify(uint16_t conn_handle, const uint8_t *payload, size_t len) {
    assert(conn_handle == 1);
    assert(len == sizeof(last_notification));
    memcpy(last_notification, payload, len);
}

static void check_status(device_state_t expected_state, device_error_mask_t expected_mask) {
    uint8_t payload[DEVICE_STATUS_PAYLOAD_LEN];
    device_status_get_payload(payload, sizeof(payload));
    assert(memcmp(payload, last_notification, sizeof(payload)) == 0);
    assert(payload[1] == expected_state);
    device_error_mask_t mask;
    memcpy(&mask, payload + 2, sizeof(mask));
    assert(mask == expected_mask);
}

int main(void) {
    device_status_init();

    temperature = 40.0f;
    thermal_protection_update(true, 40.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT);

    // Raising the saved maximum clears the previous trip at the same temperature.
    thermal_protection_update(true, 45.0f);
    check_status(DEVICE_STATE_OK, DEVICE_ERROR_NONE);

    temperature = 46.0f;
    thermal_protection_update(true, 45.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT);
    temperature = 44.0f;
    thermal_protection_update(true, 45.0f);
    check_status(DEVICE_STATE_OK, DEVICE_ERROR_NONE);

    device_status_set_error_flag(DEVICE_ERROR_INA_OFFLINE, true);
    thermal_protection_update(true, 43.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT | DEVICE_ERROR_INA_OFFLINE);
    thermal_protection_update(true, 45.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_INA_OFFLINE);

    device_status_set_error_flag(DEVICE_ERROR_INA_OFFLINE, false);
    temperature = 50.0f;
    thermal_protection_update(false, 45.0f);
    check_status(DEVICE_STATE_OK, DEVICE_ERROR_NONE);
    thermal_protection_update(true, 45.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT);
    thermal_protection_update(false, 45.0f);
    check_status(DEVICE_STATE_OK, DEVICE_ERROR_NONE);

    temperature = 50.01f;
    thermal_protection_update(false, 60.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT);
    thermal_protection_update(true, 60.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT);
    temperature = 50.0f;
    thermal_protection_update(true, 60.0f);
    check_status(DEVICE_STATE_OK, DEVICE_ERROR_NONE);

    temperature = 51.0f;
    sample_available = false;
    thermal_protection_update(false, 60.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_NTC_DISCONNECTED);
    sample_available = true;
    thermal_protection_update(false, 60.0f);
    check_status(DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT);
}
