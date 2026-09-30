/* mpu6050.c - MPU6050 六轴加速度/陀螺仪驱动（实现）
 *
 * 只依赖 ESP-IDF，不含任何业务逻辑、不读 Kconfig、不 include 本项目头文件。
 * 详见 mpu6050.h 的移植说明。
 *
 * 几个容易踩的点，都在下面的代码里对应处理：
 *   - 上电默认处于 SLEEP，必须先清 PWR_MGMT_1 才能读到数据；
 *   - 寄存器是**大端** 16 位（高字节在前），拼的时候别写反；
 *   - 内部温度传感器换算公式与整颗芯片的温漂有关，只当趋势看；
 *   - 量程越大分辨率越粗，±2g 下 1 LSB = 16384/2 = 16384 LSB/g。
 */
#include <string.h>
#include <math.h>       /* sqrtf */
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "sdkconfig.h"

#include "mpu6050.h"

static const char *TAG = "mpu6050";

/* 寄存器表 */
#define REG_SMPLRT_DIV      0x19
#define REG_CONFIG          0x1A
#define REG_GYRO_CONFIG     0x1B
#define REG_ACCEL_CONFIG    0x1C
#define REG_ACCEL_XOUT_H    0x3B    /* 数据起始寄存器，连续读 14 字节 */
#define REG_PWR_MGMT_1      0x6B
#define REG_WHO_AM_I        0x75

#define WHO_AM_I_VALUE      0x68    /* MPU6050 固定返回 0x68（与地址无关） */

/* 一次读 14 字节：ax ay az temp gx gy gz，各 2 字节大端 */
#define BURST_LEN           14

/* 量程对应的灵敏度（LSB per 物理单位），用于换算 */
static const float s_accel_lsb_per_g[] = { 16384.0f, 8192.0f, 4096.0f, 2048.0f };
static const float s_gyro_lsb_per_dps[] = { 131.0f, 65.5f, 32.8f, 16.4f };

static i2c_master_dev_handle_t s_dev = NULL;
static i2c_master_bus_handle_t s_owned_bus = NULL;   /* 仅在 mpu6050_bus_init() 后非空 */
static uint8_t s_accel_range = MPU6050_ACCEL_FS_2G;
static uint8_t s_gyro_range = MPU6050_GYRO_FS_250DPS;

/* 读一个寄存器 */
static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

/* 写一个寄存器 */
static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

/* 大端 16 位 */
static int16_t be16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] << 8 | p[1]);
}

esp_err_t mpu6050_bus_init(int sda_gpio, int scl_gpio)
{
    if (s_owned_bus) {
        return ESP_OK;                      /* 幂等 */
    }

    i2c_master_bus_config_t bus_cfg = {
        /* 默认 I2C_NUM_0：SHT30 占着 I2C_NUM_1，两个端口可以各有一对引脚，
         * 互不干扰。端口由头文件的 MPU6050_BUS_PORT 决定，可在编译选项里
         * 覆盖。改端口时务必两个驱动一起改，否则后建的那条总线会返回
         * ESP_ERR_INVALID_STATE。 */
        .i2c_port = MPU6050_BUS_PORT,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            /* 大多数 MPU6050 模块板上自带 4.7k 上拉；打开内部上拉算双保险，
             * 也避免裸芯片（无上拉）时总线一直是低电平。 */
            .enable_internal_pullup = 1,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_owned_bus),
                        TAG, "new i2c bus (SDA=%d SCL=%d)", sda_gpio, scl_gpio);

    ESP_LOGI(TAG, "I2C bus ready: port=%d SDA=%d SCL=%d",
             (int)MPU6050_BUS_PORT, sda_gpio, scl_gpio);
    return ESP_OK;
}

esp_err_t mpu6050_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (s_dev) {
        return ESP_OK;                  /* 幂等 */
    }
    /* bus == NULL 是约定的简写："用我自己建的那条总线"。
     * 这样调用方不用去 include 本驱动内部的状态，也让"想借用外部总线"
     * 和"想让驱动自己管总线"两种用法在同一个函数签名里各得其所。 */
    if (!bus) {
        bus = s_owned_bus;
    }
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus - call mpu6050_bus_init() first, or pass a "
                      "bus handle that the caller owns");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100 * 1000,     /* MPU6050 最高支持 400k，100k 更稳 */
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev),
                        TAG, "add device 0x%02X", addr);

    /* 上电默认 SLEEP=1，先唤醒。CLKSEL=1 用陀螺仪 X 轴 PLL，比内部 8MHz
     * 振荡器稳，对振动检测的零点漂移有明显好处。 */
    esp_err_t err = reg_write(REG_PWR_MGMT_1, 0x01);
    if (err != ESP_OK) {
        /* 设备没接/接线错：把句柄撤掉，保证 ready() 如实反映状态 */
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        ESP_LOGE(TAG, "wake failed at 0x%02X: %s", addr, esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));      /* 唤醒后需要一点时间稳定 */

    uint8_t who = 0;
    if (reg_read(REG_WHO_AM_I, &who) == ESP_OK && who != WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "WHO_AM_I = 0x%02X (expected 0x%02X) - wrong chip?", who,
                 WHO_AM_I_VALUE);
    }

    /* 1kHz 采样、禁用外部同步、DLPF 约 44Hz：够用且抗高频噪声 */
    reg_write(REG_CONFIG, 0x03);
    reg_write(REG_SMPLRT_DIV, 0x04);

    if (mpu6050_set_accel_range(s_accel_range) != ESP_OK) {
        return ESP_FAIL;
    }
    if (mpu6050_set_gyro_range(s_gyro_range) != ESP_OK) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "init ok: addr=0x%02X who=0x%02X accel=+-%dg gyro=+-%ddps",
             addr, who,
             2 << s_accel_range,
             (int)s_gyro_lsb_per_dps[s_gyro_range] == 131 ? 250 :
             (int)s_gyro_lsb_per_dps[s_gyro_range] == 65  ? 500 :
             (int)s_gyro_lsb_per_dps[s_gyro_range] == 32  ? 1000 : 2000);
    return ESP_OK;
}

esp_err_t mpu6050_deinit(void)
{
    if (!s_dev) {
        return ESP_OK;
    }
    esp_err_t err = i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

esp_err_t mpu6050_bus_deinit(void)
{
    mpu6050_deinit();
    if (s_owned_bus) {
        i2c_del_master_bus(s_owned_bus);
        s_owned_bus = NULL;
    }
    return ESP_OK;
}

bool mpu6050_ready(void)
{
    return s_dev != NULL;
}

esp_err_t mpu6050_set_accel_range(uint8_t range)
{
    if (!s_dev || range > MPU6050_ACCEL_FS_16G) {
        return ESP_ERR_INVALID_ARG;
    }
    /* AFS_SEL 在 bit[4:3]，低 5 位保留（含自检位，保持 0） */
    esp_err_t err = reg_write(REG_ACCEL_CONFIG, (uint8_t)(range << 3));
    if (err == ESP_OK) {
        s_accel_range = range;
    }
    return err;
}

esp_err_t mpu6050_set_gyro_range(uint8_t range)
{
    if (!s_dev || range > MPU6050_GYRO_FS_2000DPS) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = reg_write(REG_GYRO_CONFIG, (uint8_t)(range << 3));
    if (err == ESP_OK) {
        s_gyro_range = range;
    }
    return err;
}

bool mpu6050_read(mpu6050_data_t *out)
{
    if (!s_dev) {
        return false;
    }

    uint8_t raw[BURST_LEN];
    esp_err_t err = i2c_master_transmit_receive(s_dev, (uint8_t[]){ REG_ACCEL_XOUT_H }, 1,
                                                raw, sizeof(raw), 100);
    if (err != ESP_OK) {
        /* 偶发 I2C 错误（总线被拉低、供电抖动）在面包板上很常见：
         * 重新唤醒一次再试，失败才真的返回 false。 */
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        reg_write(REG_PWR_MGMT_1, 0x01);
        vTaskDelay(pdMS_TO_TICKS(10));
        err = i2c_master_transmit_receive(s_dev, (uint8_t[]){ REG_ACCEL_XOUT_H }, 1,
                                          raw, sizeof(raw), 100);
        if (err != ESP_OK) {
            return false;
        }
    }

    if (!out) {
        return true;
    }

    const float a = s_accel_lsb_per_g[s_accel_range];
    const float g = s_gyro_lsb_per_dps[s_gyro_range];

    out->ax = be16(&raw[0]) / a;
    out->ay = be16(&raw[2]) / a;
    out->az = be16(&raw[4]) / a;
    out->temp_c = be16(&raw[6]) / 340.0f + 36.53f;
    out->gx = be16(&raw[8]) / g;
    out->gy = be16(&raw[10]) / g;
    out->gz = be16(&raw[12]) / g;
    return true;
}

float mpu6050_accel_deviation_mg(const mpu6050_data_t *d)
{
    if (!d) {
        return 0.0f;
    }
    /* 合加速度 mag = sqrt(ax²+ay²+az²)，静止时约等于 1g。
     * 与 1g 的差取绝对值即"偏离程度"，单位换算成 mg。 */
    float mag = sqrtf(d->ax * d->ax + d->ay * d->ay + d->az * d->az);
    float dev = mag - 1.0f;
    return (dev < 0.0f ? -dev : dev) * 1000.0f;
}
