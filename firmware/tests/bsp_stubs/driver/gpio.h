#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_MODE_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE, GPIO_INTR_LOW_LEVEL = 4 } gpio_int_type_t;
typedef enum { GPIO_PULLUP_ONLY, GPIO_FLOATING = 3 } gpio_pull_mode_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;
typedef void (*gpio_isr_t)(void *);
esp_err_t gpio_config(const gpio_config_t *);
int gpio_get_level(gpio_num_t);
esp_err_t gpio_set_pull_mode(gpio_num_t, gpio_pull_mode_t);
esp_err_t gpio_set_intr_type(gpio_num_t, gpio_int_type_t);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_isr_handler_add(gpio_num_t, gpio_isr_t, void *);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_intr_enable(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
esp_err_t gpio_wakeup_enable(gpio_num_t, gpio_int_type_t);
esp_err_t gpio_wakeup_disable(gpio_num_t);
