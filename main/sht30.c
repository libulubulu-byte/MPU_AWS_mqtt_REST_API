/* sht30.c - SHT30 温湿度传感器驱动（I2C）
 *
 * 单次测量命令 0x2400（高重复性、非时钟拉伸模式）。
 * 非时钟拉伸：SHT30 收到命令后不拉低 SCL，避免 i2c_master 驱动的
 * 时钟拉伸超时/接收中断竞态，改为发出命令后主动延时等测量结束。
 *
 * 读取 6 字节：
 *   温度高8位 / 温度低8位 / 温度CRC / 湿度高8位 / 湿度低8位 / 湿度CRC
 * 换算：
 *   温度 = -45 + 175 * (raw_t / 65535)
 *   湿度 = 100 * (raw_h / 65535)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"

#include "sht30.h"

static const char *TAG = "sht30";

#define SHT30_I2C_ADDR      0x44
#define SHT30_CMD_MEASURE   0x2400
#define SHT30_MEASURE_WAIT_MS 20    /* 高重复性测量约 12.5ms */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* CRC-8：多项式 0x31，初始 0xFF */
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sht30_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        /* I2C_NUM_1：MPU6050 在 GPIO15/16 上占着 I2C_NUM_0，两个端口各用
         * 一对引脚，互不干扰。用 0 会因为端口已被占用而返回
         * ESP_ERR_INVALID_STATE。两个端口的分工见 env_sensors.h。 */
        .i2c_port = I2C_NUM_1,
        .sda_io_num = CONFIG_APP_SHT30_SDA_GPIO,
        .scl_io_num = CONFIG_APP_SHT30_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "new i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT30_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "add sht30");

    ESP_LOGI(TAG, "SHT30 init ok: I2C1 SDA=%d SCL=%d addr=0x%02X",
             CONFIG_APP_SHT30_SDA_GPIO, CONFIG_APP_SHT30_SCL_GPIO, SHT30_I2C_ADDR);
    return ESP_OK;
}

esp_err_t sht30_deinit(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    return ESP_OK;
}

/* 总线句柄外借：留给"在 SHT30 那对引脚（GPIO12/13）上再挂一个小器件"用。
 * 同一条 I2C 总线不能被初始化两次，谁先初始化谁持有句柄，后挂的设备只能
 * 通过本函数借用。当前 MPU6050 不借这条总线（它自己持有 I2C_NUM_0）。 */
i2c_master_bus_handle_t sht30_get_bus(void)
{
    return s_bus;
}

bool sht30_read(float *temp_c, float *hum_pct)
{
    if (!s_dev) {
        return false;
    }

    const uint8_t cmd[2] = { (uint8_t)(SHT30_CMD_MEASURE >> 8), (uint8_t)(SHT30_CMD_MEASURE & 0xFF) };
    uint8_t data[6] = { 0 };

    esp_err_t err = i2c_master_transmit(s_dev, cmd, sizeof(cmd), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "transmit failed: %s", esp_err_to_name(err));
        return false;
    }

    /* 非时钟拉伸模式：主动等待测量完成，之后 receive 不会遇到时钟拉伸 */
    vTaskDelay(pdMS_TO_TICKS(SHT30_MEASURE_WAIT_MS));

    err = i2c_master_receive(s_dev, data, sizeof(data), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "receive failed: %s", esp_err_to_name(err));
        return false;
    }

    if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "crc mismatch");
        return false;
    }

    uint16_t raw_t = (uint16_t)((data[0] << 8) | data[1]);
    uint16_t raw_h = (uint16_t)((data[3] << 8) | data[4]);

    if (temp_c) {
        *temp_c = -45.0f + 175.0f * (float)raw_t / 65535.0f;
    }
    if (hum_pct) {
        *hum_pct = 100.0f * (float)raw_h / 65535.0f;
    }
    return true;
}
