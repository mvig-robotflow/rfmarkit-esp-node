#include "apps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include <string.h>

#include "settings.h"
#include "types.h"
#include "esp_log.h"
#include "gy95.h"
#include "functions.h"

static const char* TAG = "GY95";
// static portMUX_TYPE s_gy95_mux = portMUX_INITIALIZER_UNLOCKED;

#define ENTER_CONFIGURATION(p_gy)    \
    xSemaphoreTake(p_gy->mux, portMAX_DELAY);\
    uart_write_bytes((p_gy->port), (uint8_t*) "\xa4\x06\x03\x01\xae", 5);\
    vTaskDelay(200 / portTICK_PERIOD_MS)

#define EXIT_CONFIGURATION(p_gy)    \
    xSemaphoreGive(p_gy->mux);\
    uart_write_bytes((p_gy->port), (uint8_t*) "\xa4\x06\x03\x00\xad", 5);\
    vTaskDelay(50 / portTICK_PERIOD_MS)


void uart_service_init(int port, int rx, int tx, int rts, int cts) {
    uart_config_t uart_config = {
        .baud_rate = 115200, // TODO: Magic baud_rate
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,

    };
    int intr_alloc_flags = 0;
    ESP_LOGI(TAG, "Initiate uart service at port %d, rx:%d, tx:%d", port, rx, tx);
    ESP_ERROR_CHECK(uart_driver_install(port, 512, 0, 0, NULL, intr_alloc_flags)); // TODO: Magic rx buffer len
    ESP_ERROR_CHECK(uart_param_config(port, &uart_config));

    ESP_ERROR_CHECK(uart_set_pin(port, tx, rx, rts, cts));
}


void gy95_msp_init(gy95_t* p_gy) {
    uart_service_init(p_gy->port, p_gy->rx_pin, p_gy->tx_pin, p_gy->rts_pin, p_gy->cts_pin);

    gpio_config_t io_conf = {
        .intr_type = GPIO_PIN_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pin_bit_mask = (1ULL << p_gy->ctrl_pin),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };

    gpio_config(&io_conf);
    gpio_set_level(p_gy->ctrl_pin, 0);
    bool ret = rtc_gpio_is_valid_gpio(p_gy->ctrl_pin);
    if (ret) {
        ESP_LOGI(TAG, "GPIO: %d is valid rtc gpio", p_gy->ctrl_pin);
    } else {
        ESP_LOGW(TAG, "GPIO: %d is not valid rtc gpio", p_gy->ctrl_pin);
    }
}

void gy95_init(gy95_t* p_gy,
               int port,
               int ctrl_pin,
               int rx_pin,
               int tx_pin,
               int rts_pin,
               int cts_pin,
               int addr
) {
    p_gy->port = port;
    p_gy->ctrl_pin = ctrl_pin;
    p_gy->rx_pin = rx_pin;
    p_gy->tx_pin = tx_pin;
    p_gy->rts_pin = rts_pin;
    p_gy->cts_pin = cts_pin;

    p_gy->addr = addr;
    p_gy->cursor = 0;
    p_gy->start_reg = 0;
    p_gy->length = 0;
    p_gy->flag = 0;

    p_gy->mux = xSemaphoreCreateMutex();
    if (p_gy->mux == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        esp_restart();
    }

    bzero(p_gy->buf, GY95_MSG_LEN);

}

/**
 * @brief Clean GY95 buffer
 *
 * @param p_gy
 */
void gy95_clean(gy95_t* p_gy) {
    bzero(p_gy->buf, GY95_MSG_LEN);
    p_gy->cursor = 0;
    p_gy->start_reg = 0;
    p_gy->length = 0;
    p_gy->flag = 0;
}

#define CONFIG_GY95_MAX_CHECK_LEN 1024
#define CONFIG_GY95_MAX_CHECK_TIMEOUT 3072
static esp_err_t gy95_check_echo(gy95_t* p_gy, uint8_t* msg, int len) {
    /** Clean old message **/
    gy95_clean(p_gy);
    int cnt = CONFIG_GY95_MAX_CHECK_LEN;
    TickType_t start_tick = xTaskGetTickCount();
    while (cnt > 0) {
        if (xTaskGetTickCount() - start_tick > CONFIG_GY95_MAX_CHECK_TIMEOUT) {
            break;
        }
        uart_read_bytes(p_gy->port, &p_gy->buf[p_gy->cursor], 1, 0xFF);
#if CONFIG_EN_GY95_DEBUG
        printf("0x%x.", p_gy->buf[p_gy->cursor]);
#endif
        if (p_gy->buf[p_gy->cursor] != msg[p_gy->cursor]) {
            gy95_clean(p_gy);
            ESP_LOGD(TAG, "GYT95 reset buffer");
            continue;
        } else {
            ++p_gy->cursor;
        }
        if (p_gy->cursor >= len) {
            return ESP_OK;
        }
        --cnt;
    }
    return ESP_FAIL;
}

/**
 * @brief Send msg with chksum appended
 *
 * @param p_gy
 * @param msg
 * @param len
 */
esp_err_t gy95_send(gy95_t* p_gy, uint8_t* msg, int len) {
    // #if ! CONFIG_MULTI_CORE
    //     taskENTER_CRITICAL(&s_gy95_mux);
    // #endif

    // taskENTER_CRITICAL(&s_gy95_mux);
    ENTER_CONFIGURATION(p_gy);

    if (len <= 0) {
        len = strlen((char*)msg);
    }
    long int sum = 0;
    char chksum = 0;
    for (int idx = 0; idx < len; ++idx) {
        sum += msg[idx];
    }
    chksum = sum % 0x100;
    // taskEXIT_CRITICAL(&s_gy95_mux);

    uart_write_bytes(p_gy->port, msg, len);
    uart_write_bytes(p_gy->port, &chksum, 1);

    esp_err_t err = gy95_check_echo(p_gy, msg, len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "GY95 Echo Succeed");
    } else {
        ESP_LOGE(TAG, "GY95 Echo Failed");
    }

    EXIT_CONFIGURATION(p_gy);

    return err;
}

esp_err_t gy95_setup(gy95_t* p_gy) {

    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Set rate to 100hz");
    err = (err && gy95_send(p_gy, (uint8_t*)"\xa4\x06\x02\x02", 4));

    ESP_LOGI(TAG, "Set calibration method"); // TODO: experimental
    err = (err && gy95_send(p_gy, (uint8_t*)"\xa4\x06\x06\x73", 4));

    ESP_LOGI(TAG, "Set mount to horizontal");
    err = (err && gy95_send(p_gy, (uint8_t*)"\xa4\x06\x07\x8b", 4));

    return err;
}

esp_err_t gy95_cali_acc(gy95_t* p_gy) {

    ESP_LOGI(TAG, "Re-run setup");
    esp_err_t err = gy95_setup(p_gy);

    ESP_LOGI(TAG, "Gyro-Accel calibrate");
    gy95_send(p_gy, (uint8_t*)"\xa4\x06\x05\x57", 4);

    ESP_LOGI(TAG, "Save module configuration");
    /** Save module configuration **/
    gy95_send(p_gy, (uint8_t*)"\xa4\x06\x05\x55", 4);

    return err;
}
// if self.ser.writable():
//     self.ser.write(append_chksum(bytearray([0xa4, 0x06, 0x02, 0x02])))  # Rate 100Hz
//     time.sleep(0.1)
//     self.ser.write(append_chksum(bytearray([0xa4, 0x06, 0x03, 0x00])))  # Auto update
//     time.sleep(0.1)
//     self.ser.write(append_chksum(bytearray([0xa4, 0x06, 0x07, 0x8b])))  # Mount horizontally
//     time.sleep(0.1)
//     self.ser.write(append_chksum(bytearray([0xa4, 0x06, 0x05, 0x57])))  # Calibrate
//     time.sleep(7)

void gy95_cali_mag(gy95_t* p_gy) {
    ESP_LOGI(TAG, "Mag calibrate");
    /** Start calibration **/
    gy95_send(p_gy, (uint8_t*)"\xa4\x06\x05\x58", 4);
    ESP_LOGI(TAG, "Point the IMU to all directions in next 15 seconds");

    vTaskDelay(30000 / portTICK_PERIOD_MS); // TODO: Magic delay

    /** Stop calibration **/
    gy95_send(p_gy, (uint8_t*)"\xa4\x06\x05\x59", 4);

    vTaskDelay(200 / portTICK_PERIOD_MS); // TODO: Magic delay
    /** Save calibration result**/
    gy95_send(p_gy, (uint8_t*)"\xa4\x06\x05\x5A", 4);

    vTaskDelay(200 / portTICK_PERIOD_MS); // TODO: Magic delay

    /** Save module configuration **/
    gy95_send(p_gy, (uint8_t*)"\xa4\x06\x05\x55", 4);

    vTaskDelay(200 / portTICK_PERIOD_MS); // TODO: Magic delay

}

esp_err_t gy95_cali_reset(gy95_t* p_gy) {
    gy95_send(p_gy, (uint8_t*)"\xa4\x06\x05\xaa", 4);
    esp_err_t err = gy95_setup(p_gy);
    vTaskDelay(200 / portTICK_PERIOD_MS); // TODO: Magic delay
    return err;
}

bool gy95_chksum(gy95_t* p_gy) {
    long int sum = 0;
    for (int idx = 0; idx < p_gy->cursor; ++idx) {
        sum += p_gy->buf[idx];
    }
    return (sum % 0x100 == p_gy->buf[p_gy->cursor]) ? true : false;
}

#define CONFIG_GY95_MAXFAILED_BYTES 1924
void gy95_read(gy95_t* p_gy) {
    /** Acqurie lock **/
    xSemaphoreTake(p_gy->mux, portMAX_DELAY);

    gy95_clean(p_gy);
    int failed_bytes = 0;
    while (failed_bytes < CONFIG_GY95_MAXFAILED_BYTES) {
        uart_read_bytes(p_gy->port, &p_gy->buf[p_gy->cursor], 1, 0xFF);
        ESP_LOGD(TAG, "%d:%d:%d\t", p_gy->port, p_gy->buf[p_gy->cursor], p_gy->cursor);

        switch (p_gy->cursor) {
        case 0:
            if (p_gy->buf[p_gy->cursor] != p_gy->addr) {
                failed_bytes += p_gy->cursor;
                gy95_clean(p_gy);
                ESP_LOGD(TAG, "GYT95 reset buffer");
                continue;
            }
            break;
        case 1:
            if (p_gy->buf[p_gy->cursor] != GY95_READ_OP) {
                failed_bytes += p_gy->cursor;
                gy95_clean(p_gy);
                ESP_LOGD(TAG, "GYT95 reset buffer");
                continue;
            }
            break;
        case 2:
            if (p_gy->buf[p_gy->cursor] < GY95_REG_THRESH) {
                p_gy->start_reg = p_gy->buf[p_gy->cursor];
            } else {
                failed_bytes += p_gy->cursor;
                gy95_clean(p_gy);
                ESP_LOGD(TAG, "GYT95 reset buffer");
                continue;
            }
            break;
        case 3:
            if (p_gy->start_reg + (p_gy->buf[p_gy->cursor]) < GY95_REG_THRESH) {
                p_gy->length = p_gy->buf[p_gy->cursor];
            } else {
                failed_bytes += p_gy->cursor;
                gy95_clean(p_gy);
                ESP_LOGD(TAG, "GYT95 reset buffer");
                continue;
            }
            break;
        default:
            if (p_gy->length + 4 == p_gy->cursor) {
                p_gy->flag = true;
            }
        }

        if (p_gy->flag) {
            p_gy->flag = false;
            if (gy95_chksum(p_gy)) {
                break;
            } else {
                ESP_LOGI(TAG, "GYT95 reset buffer");
                failed_bytes += p_gy->cursor;
                gy95_clean(p_gy);
            }
        } else {
            ++p_gy->cursor;
        }

    }

    if (failed_bytes >= CONFIG_GY95_MAXFAILED_BYTES) {
        gy95_clean(p_gy);
    }

    xSemaphoreGive(p_gy->mux);
    /** Failed to read gy95 **/
}

void gy95_enable(gy95_t* p_gy) {
    gpio_set_level(p_gy->ctrl_pin, 0);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    int ret = gpio_get_level(p_gy->ctrl_pin);
    ESP_LOGI(TAG, "GY95 control pin %d is %s", p_gy->ctrl_pin, ret ? "HIGH" : "LOW");
}

void gy95_disable(gy95_t* p_gy) {
    gpio_set_level(p_gy->ctrl_pin, 1);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    int ret = gpio_get_level(p_gy->ctrl_pin);
    ESP_LOGI(TAG, "GY95 control pin %d is %s", p_gy->ctrl_pin, ret ? "HIGH" : "LOW");
}