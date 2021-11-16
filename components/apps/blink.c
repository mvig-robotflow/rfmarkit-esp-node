#include "apps.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "driver/timer.h"
#include "esp_log.h"

#include "settings.h"

#define TIMER_DIVIDER         16  //  Hardware timer clock divider
#define TIMER_SCALE           (TIMER_BASE_CLK / TIMER_DIVIDER)  // convert counter value to seconds
#define LED_ON() gpio_set_level(g_blink_pin, CONFIG_BLINK_LED_ENABLE_VALUE)
#define LED_OFF() gpio_set_level(g_blink_pin, !CONFIG_BLINK_LED_ENABLE_VALUE)

#define CONFIG_BLINK_SYNC_SEQ_LEN 4
#ifdef CONFIG_BLINK_EN_CHKSUM
#define CONFIG_BLINK_CHKSUM_SEQ_LEN 4
#else 
#define CONFIG_BLINK_CHKSUM_SEQ_LEN 0
#endif

static char s_blink_seq[CONFIG_BLINK_SYNC_SEQ_LEN + CONFIG_BLINK_SEQ_LEN + CONFIG_BLINK_CHKSUM_SEQ_LEN];
// [ 4bit sync | 16 bit data | 4 bit chksum]

static int s_blink_idx;
uint8_t g_blink_pin;

static const char* TAG = "app_blink";

bool blink_timeout(void* args) {
    if (s_blink_idx >= CONFIG_BLINK_SEQ_LEN) s_blink_idx = 0;
    gpio_set_level(g_blink_pin, s_blink_seq[s_blink_idx] ? 1 : 0);
    s_blink_idx++;
    // printf("%d", s_blink_seq[s_blink_idx]);

    return false;
}

static bool get_flag(uint8_t* arr, int item) {
    return (arr[item / 8] & (1 << item % 8)) >> (item % 8);
}

void app_blink_init() {
    /** Read blink sequence from nvs **/
    nvs_handle_t blink_handle;
    uint8_t seq;
    nvs_open("blink", NVS_READWRITE, &blink_handle);

    /** FIXME: Temporarily suppress pin config
    nvs_get_u8(blink_handle, "pin", &g_blink_pin);
    if (g_blink_pin != CONFIG_BLINK_BLUE_PIN && g_blink_pin != CONFIG_BLINK_GREEN_PIN && g_blink_pin != CONFIG_BLINK_RED_PIN) {
        g_blink_pin = CONFIG_BLINK_DEFAULT_PIN;
        nvs_set_u8(blink_handle, "pin", g_blink_pin); // TODO: Magic Name
        nvs_commit(blink_handle);
    }
    **/
    g_blink_pin = CONFIG_BLINK_DEFAULT_PIN;
    nvs_get_u8(blink_handle, "seq", &seq);
    ESP_LOGI(TAG, "Blinking pin: %d", g_blink_pin);
    ESP_LOGI(TAG, "Blinking sequence: %d", seq);
    s_blink_idx = 0;

    bzero(s_blink_seq, sizeof(s_blink_seq));
    bool pattern[2] = { 1,0 };
    bool value = 0;

    /** Init GPIO **/
    gpio_config_t io_config = {
        .pin_bit_mask = (1ull << CONFIG_BLINK_RED_PIN) | (1ull << CONFIG_BLINK_GREEN_PIN) | (1ull << CONFIG_BLINK_BLUE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_config);

    /** After init, light up the led **/
    LED_ALLOFF();
    LED_ON();

    ESP_LOGI(TAG, "Sequence Number: %d", seq);

    /** fill sync bits **/
    s_blink_seq[0] = 1;
    s_blink_seq[1] = 1;
    s_blink_seq[2] = 1;
    s_blink_seq[3] = 0;

    /** fill data bits **/
    int n_ones = 0;
    for (int idx = 0; idx < 8; ++idx) {
        value = get_flag(&seq, 8 - idx);
        ESP_LOGD(TAG, "value: %d\n", value);
        if (value) {
            pattern[0] = !pattern[0];
            pattern[1] = !pattern[1];
            ++n_ones;
        }
        s_blink_seq[CONFIG_BLINK_SYNC_SEQ_LEN + idx * 2] = pattern[0];
        ESP_LOGD(TAG, "s_blink_seq[%d]=%d\n", idx * 2, pattern[0]);
        s_blink_seq[CONFIG_BLINK_SYNC_SEQ_LEN + idx * 2 + 1] = pattern[1];
        ESP_LOGD(TAG, "s_blink_seq[%d]=%d\n", idx * 2 + 1, pattern[1]);
    }

#ifdef CONFIG_BLINK_EN_CHKSUM
#else 
#endif

    ESP_LOGW(TAG, "# -------- Begin of blink sequence -------- #");
    for (int idx = 0; idx < sizeof(s_blink_seq); ++idx) {
        printf("%d ", s_blink_seq[idx]);
    }
    printf("\n");
    ESP_LOGW(TAG, "# --------- End of blink sequence --------- #");

    // Arm timers
    timer_config_t config = {
            .alarm_en = 1,
            .counter_en = 0,
            .counter_dir = TIMER_COUNT_UP,
            .auto_reload = 1,
            .divider = TIMER_DIVIDER,
    };
    timer_init(0, 0, &config);
    timer_set_counter_value(CONFIG_BLINK_TIMER_GROUP, CONFIG_BLINK_TIMER_IDX, 0x00ull);
    timer_set_alarm_value(CONFIG_BLINK_TIMER_GROUP, CONFIG_BLINK_TIMER_IDX, TIMER_SCALE * CONFIG_BLINK_INTERVAL_MS / 1000);
    timer_enable_intr(CONFIG_BLINK_TIMER_GROUP, CONFIG_BLINK_TIMER_IDX);

    timer_isr_callback_add(CONFIG_BLINK_TIMER_GROUP, CONFIG_BLINK_TIMER_IDX, blink_timeout, NULL, ESP_INTR_FLAG_IRAM);
}

void app_blink_start() {
    ESP_LOGI(TAG, "Timer started");
    ESP_LOGW(TAG, "# -------- Begin of blink sequence -------- #");
    for (int idx = 0; idx < CONFIG_BLINK_SEQ_LEN; ++idx) {
        printf("%d ", s_blink_seq[idx]);
    }
    printf("\n");
    ESP_LOGW(TAG, "# --------- End of blink sequence --------- #");
    timer_start(CONFIG_BLINK_TIMER_GROUP, CONFIG_BLINK_TIMER_IDX);
}

void app_blink_stop() {
    ESP_LOGI(TAG, "Timer stopped");
    timer_pause(CONFIG_BLINK_TIMER_GROUP, CONFIG_BLINK_TIMER_IDX);
    LED_ON();
}