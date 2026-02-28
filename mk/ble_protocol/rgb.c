#include "rgb.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"

#define RGB_GPIO 8
#define RGB_RESOLUTION_HZ 10000000
#define BLINK_PERIOD_MS 150
#define BLINK_STEPS 4
#define BLINK_DELAY_MS 120
#define NOTIFY_PULSE_MS 120

static const char *TAG = "rgb";
static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_bytes_encoder = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static rmt_symbol_word_t s_reset_symbol;
static bool s_pairing = false;
static bool s_connected = false;
static bool s_blink_active = false;
static bool s_notify_active = false;
static bool s_blink_pending = false;
static int s_blink_step = 0;
static uint8_t s_blink_r = 255;
static uint8_t s_blink_g = 0;
static uint8_t s_blink_b = 0;
static esp_timer_handle_t s_blink_timer = NULL;
static esp_timer_handle_t s_blink_delay_timer = NULL;
static esp_timer_handle_t s_notify_timer = NULL;

static void rgb_send(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_chan || !s_bytes_encoder || !s_copy_encoder) return;
    uint8_t sr = (uint8_t)((r * 51) / 255);
    uint8_t sg = (uint8_t)((g * 51) / 255);
    uint8_t sb = (uint8_t)((b * 51) / 255);
    uint8_t data[3] = { sg, sr, sb };
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,
        .flags.queue_nonblocking = 0
    };
    esp_err_t err = rmt_transmit(s_chan, s_bytes_encoder, data, sizeof(data), &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_transmit bytes failed: %d", err);
        return;
    }
    err = rmt_transmit(s_chan, s_copy_encoder, &s_reset_symbol, sizeof(s_reset_symbol), &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_transmit reset failed: %d", err);
        return;
    }
    rmt_tx_wait_all_done(s_chan, 100);
}

static void rgb_apply(void) {
    if (!s_chan) return;
    if (s_blink_active) return;
    if (s_blink_pending) {
        rgb_send(0, 0, 0);
        return;
    }
    if (s_notify_active) {
        rgb_send(0, 255, 0);
        return;
    }
    uint8_t r = 0, g = 0, b = 0;
    if (s_pairing) {
        g = 255;
    } else if (s_connected) {
        r = 255;
    }
    rgb_send(r, g, b);
}

static void notify_cb(void *arg) {
    (void)arg;
    s_notify_active = false;
    rgb_apply();
}

static void blink_cb(void *arg) {
    (void)arg;
    if (!s_chan) return;
    if (!s_blink_active) return;

    if ((s_blink_step % 2) == 0) {
        rgb_send(s_blink_r, s_blink_g, s_blink_b);
    } else {
        rgb_send(0, 0, 0);
    }
    s_blink_step++;
    if (s_blink_step >= BLINK_STEPS) {
        s_blink_active = false;
        esp_timer_stop(s_blink_timer);
        rgb_apply();
    }
}

static void blink_delay_cb(void *arg) {
    (void)arg;
    if (!s_chan) return;
    if (!s_blink_pending) return;
    s_blink_pending = false;
    s_blink_active = true;
    s_blink_step = 0;
    esp_timer_stop(s_blink_timer);
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_blink_timer, BLINK_PERIOD_MS * 1000));
}

void rgb_init(void) {
    if (s_chan) return;

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = RGB_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RGB_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .intr_priority = 0,
        .flags.invert_out = 0,
        .flags.with_dma = 0,
        .flags.io_loop_back = 0,
        .flags.io_od_mode = 0,
        .flags.allow_pd = 0
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_new_tx_channel failed: %d", err);
        s_chan = NULL;
        return;
    }

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 8
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 7,
            .level1 = 0,
            .duration1 = 6
        },
        .flags.msb_first = 1
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_bytes_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_new_bytes_encoder failed: %d", err);
        s_bytes_encoder = NULL;
        return;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &s_copy_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_new_copy_encoder failed: %d", err);
        s_copy_encoder = NULL;
        return;
    }

    s_reset_symbol = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = (uint16_t)(RGB_RESOLUTION_HZ / 1000000 * 50),
        .level1 = 0,
        .duration1 = 0
    };

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_enable failed: %d", err);
    }

    esp_timer_create_args_t targs = {
        .callback = blink_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rgb_blink"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_blink_timer));

    esp_timer_create_args_t dargs = {
        .callback = blink_delay_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rgb_bdelay"
    };
    ESP_ERROR_CHECK(esp_timer_create(&dargs, &s_blink_delay_timer));

    esp_timer_create_args_t n_args = {
        .callback = notify_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rgb_notify"
    };
    ESP_ERROR_CHECK(esp_timer_create(&n_args, &s_notify_timer));
}

void rgb_set_pairing(bool enabled) {
    s_pairing = enabled;
    rgb_apply();
}

void rgb_set_connected(bool connected) {
    s_connected = connected;
    rgb_apply();
}

void rgb_blink_reset(void) {
    if (!s_chan) return;
    s_blink_active = true;
    s_blink_step = 0;
    s_blink_r = 255;
    s_blink_g = 0;
    s_blink_b = 0;
    s_blink_pending = false;
    esp_timer_stop(s_blink_timer);
    esp_timer_stop(s_blink_delay_timer);
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_blink_timer, BLINK_PERIOD_MS * 1000));
}

void rgb_notify_pulse(void) {
    if (!s_chan) return;
    if (s_blink_active) return;
    s_notify_active = true;
    rgb_send(0, 255, 0);
    esp_timer_stop(s_notify_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_notify_timer, NOTIFY_PULSE_MS * 1000));
}

void rgb_blink_pair_success(void) {
    if (!s_chan) return;
    s_blink_r = 255;
    s_blink_g = 165;
    s_blink_b = 0;
    s_blink_active = false;
    s_blink_pending = true;
    s_blink_step = 0;
    rgb_send(0, 0, 0);
    esp_timer_stop(s_blink_timer);
    esp_timer_stop(s_blink_delay_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_blink_delay_timer, BLINK_DELAY_MS * 1000));
}
