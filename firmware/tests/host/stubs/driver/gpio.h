#ifndef HOST_DRIVER_GPIO_H
#define HOST_DRIVER_GPIO_H

#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_3 3
#define GPIO_NUM_10 10

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0

esp_err_t gpio_set_level(gpio_num_t gpio_num, int level);
int gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_config(const gpio_config_t *config);
void gpio_deep_sleep_hold_dis(void);
esp_err_t gpio_hold_dis(gpio_num_t gpio_num);
esp_err_t gpio_hold_en(gpio_num_t gpio_num);
void gpio_deep_sleep_hold_en(void);

#endif
