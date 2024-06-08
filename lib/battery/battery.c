//
// Created by liyutong on 2024/4/30.

#include "esp_sleep.h"
#include "esp_log.h"

#include "driver/adc.h"
#include "driver/gpio.h"

#include "battery.h"
#include "modelspec.h"

static const char *TAG = "battery         ";

esp_err_t battery_msp_init() {
    /** Init GPIO **/
    gpio_config_t io_config = {
            .pin_bit_mask = (1ull << CONFIG_BATTERY_EN_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
    };

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(CONFIG_BATTERY_READ_ADC1_CHANNEL, ADC_ATTEN_DB_0);

    gpio_config(&io_config);
    /** Self-Test **/
    int lvl = battery_read_level();
    ESP_LOGI(TAG, "battery level: %d", lvl);
    return ESP_OK;
}

int battery_read_level() {
    gpio_set_level(CONFIG_BATTERY_EN_PIN, CONFIG_BATTERY_EN_VALUE);
    battery_delay_ms(50);
    int val = adc1_get_raw(CONFIG_BATTERY_READ_ADC1_CHANNEL);
    gpio_set_level(CONFIG_BATTERY_EN_PIN, !CONFIG_BATTERY_EN_VALUE);
    return val;
}