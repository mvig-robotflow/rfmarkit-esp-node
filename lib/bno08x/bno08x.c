#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "nvs_flash.h"

#include "imu.h"
#include "bno08x.h"
#include "bno08x_driver.h"

#define bno08x_delay_ms(x) vTaskDelay((x) / portTICK_PERIOD_MS);

typedef struct {
    imu_t base;
    BNO08x driver;
} bno08x_t;

imu_interface_t g_imu = {0};
static bno08x_t s_bno08x = {0};

static const char *TAG = "imu[bno08x]";

/**
 * @brief
 * @param p_imu
 * @param p_config
 * @return
**/
static esp_err_t bno08x_init(imu_t *p_imu, __attribute__((unused)) imu_config_t *p_config) {
    bno08x_t *p_bno08x = (bno08x_t *) p_imu;
    BNO08x *p_driver = &p_bno08x->driver;

    p_imu->addr = CONFIG_BNO08X_ADDR;
    p_imu->enabled = false;

    p_imu->mutex = xSemaphoreCreateMutex();
    if (p_imu->mutex == NULL) {
        ESP_LOGE(TAG, "failed to create mutex");
        p_imu->status = IMU_STATUS_FAIL;
    } else {
        p_imu->status = IMU_STATUS_READY;
    }

    BNO08x_config_t config = {
        .io_mosi = CONFIG_BNO08X_MOSI_PIN,
        .io_miso = CONFIG_BNO08X_MISO_PIN,
        .io_sclk = CONFIG_BNO08X_SCLK_PIN,
        .io_cs = CONFIG_BNO08X_CS_PIN,
        .io_int = CONFIG_BNO08X_INT_PIN,
        .io_rst = CONFIG_BNO08X_RST_PIN,
        .io_wake = CONFIG_BNO08X_WAKE_PIN,
    };

    BNO08x_init(p_driver, &config);
    bool ret = BNO08x_initialize(p_driver);
    if (!ret) {
        ESP_LOGE(TAG, "failed to initialize BNO08x");
        p_imu->status = IMU_STATUS_FAIL;
        return ESP_FAIL;
    }
    BNO08x_enable_accelerometer(p_driver, CONFIG_BNO08X_INTERVAL_MS); // 200Hz
    BNO08x_enable_game_rotation_vector(p_driver, CONFIG_BNO08X_INTERVAL_MS);

    p_imu->initialized = true;
    return ESP_OK;
}

/**
 * @brief
 * @param p_imu
 * @param out
 * @param crc_check
 * @return
**/
esp_err_t bno08x_read(imu_t *p_imu, imu_dgram_t *out, bool crc_check) {
    BNO08x *p_driver = &((bno08x_t *) p_imu)->driver;

    float rad_acc;
    uint8_t acc;

    for (int i = 0; i < CONFIG_BNO08X_TRY_TIMES; i++) {
        if (BNO08x_data_available(p_driver)) {
            BNO08x_get_quat(p_driver, &out->imu.quat[1], &out->imu.quat[2], &out->imu.quat[3], &out->imu.quat[0], &rad_acc, &acc);
            out->imu.eul[0] = BNO08x_get_roll(p_driver);
            out->imu.eul[1] = BNO08x_get_pitch(p_driver);
            out->imu.eul[2] = BNO08x_get_yaw(p_driver);
            BNO08x_get_accel(p_driver, &out->imu.acc[0], &out->imu.acc[1], &out->imu.acc[2], &acc);
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

/**
 * @brief
 * @param p_imu
 * @param enable
 * @return
**/
esp_err_t bno08x_toggle(imu_t *p_imu, bool enable) {
    // Not implemented
    return ESP_OK;
}

/**
 * @brief
 * @param p_imu
 * @return
**/
int bno08x_is_powered_on(imu_t *p_imu) {
    // Not implemented
    return 1;
}

/**
 * @brief
 * @param p_imu
 * @return
**/
esp_err_t bno08x_self_test(imu_t *p_imu) {
    // Not implemented
    return ESP_OK;
}

/**
 * @brief
 * @param p_imu
**/
void bno08x_chip_soft_reset(imu_t *p_imu) {
    // Not implemented
}

/**
 * @brief
 * @param p_imu
**/
void bno08x_chip_hard_reset(imu_t *p_imu) {
    // Not implemented
}

/**
 * @brief
 * @param p_imu
**/
void bno08x_buffer_reset(imu_t *p_imu) {
    // Not implemented
}

/**
 * @brief
 * @param p_imu
 * @return
**/
int64_t bno08x_get_buffer_delay(imu_t *p_imu) {
    // Not implemented
    return 0;
}

/**
 * @brief
 * @param p_imu
 * @param out
 * @param len
 * @return
**/
size_t bno08x_read_bytes(imu_t *p_imu, uint8_t *out, size_t len) {
    // Not implemented
    return 0;
}

/**
 * @brief
 * @param p_imu
 * @param in
 * @param len
 * @return
**/
esp_err_t bno08x_write_bytes(imu_t *p_imu, void *in, size_t len) {
    // Not implemented
    return ESP_OK;
}

/**
 *
 * @param p_interface
 * @param p_config
**/
void imu_interface_init(imu_interface_t *p_interface, __attribute__((unused)) imu_config_t *p_config) {
    p_interface->p_imu = (imu_t *) &s_bno08x;
    if (p_interface->p_imu->initialized) {
        ESP_LOGW(TAG, "IMU already initialized");
        return;
    } else {
        bno08x_init(p_interface->p_imu, NULL);
    }

    p_interface->init = bno08x_init;
    p_interface->read = bno08x_read;
    p_interface->toggle = bno08x_toggle;
    p_interface->is_powered_on = bno08x_is_powered_on;
    p_interface->self_test = bno08x_self_test;
    p_interface->chip_soft_reset = bno08x_chip_soft_reset;
    p_interface->chip_hard_reset = bno08x_chip_hard_reset;
    p_interface->buffer_reset = bno08x_buffer_reset;
    p_interface->get_buffer_delay = bno08x_get_buffer_delay;
    p_interface->read_bytes = bno08x_read_bytes;
    p_interface->write_bytes = bno08x_write_bytes;
}