/* mpu6050.h - MPU6050 六轴加速度/陀螺仪驱动（I2C）
 *
 * 移植说明：本文件与 mpu6050.c 只依赖 ESP-IDF（driver/i2c_master.h 等）。
 * 驱动本身不 include 任何本项目头文件、不做任何业务判断；唯一读的宏是
 * 可选的 MPU6050_BUS_PORT（见下方总线归属说明），没定义时走默认端口。
 * 拷到别的工程里，配好 CMakeLists 的 REQUIRES driver 即可直接编译。
 *
 * 总线归属：驱动提供两种用法，二选一。
 *
 *   1) 自己持有总线（本项目用法）：调 mpu6050_bus_init(SDA, SCL) 建一条
 *      驱动自己管的总线，再调 mpu6050_init(NULL, addr) 挂上去。销毁用
 *      mpu6050_bus_deinit()。适合 MPU6050 独占一对引脚的情况。
 *
 *   2) 借用别人的总线：不调 mpu6050_bus_init()，直接把别人已建好的
 *      句柄传给 mpu6050_init(bus, addr)。此时驱动只 add_device，
 *      绝不销毁那条总线（它不归驱动所有）。
 *
 * ⚠️ 本项目曾经用的是第 2 种：MPU6050 借 SHT30 在 GPIO12/13 上的总线。
 * 但实际接线里 MPU6050 在另外一对引脚上，借来的总线上自然找不到它，
 * 表现为串口里 "imu --... vib 0" 永远是读不到，而日志只有一句很容易
 * 看漏的 "MPU6050 not found on the shared I2C bus"。换成第 1 种之后
 * 这种"接对了线却不工作"的情况就不会再出现。
 *
 * ⚠️ 引脚别选 GPIO41/42：ESP32-S3 上那是 USB 的 D-/D+，而本工程开着
 * USB Serial/JTAG 二级控制台（sdkconfig 里 CONFIG_ESP_CONSOLE_SECONDARY_
 * USB_SERIAL_JTAG=y）。I2C 挂到 41/42 会和 USB 抢同一对脚，表现为"开机
 * 能读，过一会儿开始 NACK"——传感器时好时坏、串口刷 i2c.master nack。
 * 默认改成 GPIO15/16 就是为了避开这个坑。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MPU6050 的 7 位地址：AD0 接地为 0x68，接高为 0x69 */
#define MPU6050_ADDR_AD0_LOW    0x68
#define MPU6050_ADDR_AD0_HIGH   0x69

/* 加速度满量程（对应 ACCEL_CONFIG 的 AFS_SEL[1:0]） */
#define MPU6050_ACCEL_FS_2G     0
#define MPU6050_ACCEL_FS_4G     1
#define MPU6050_ACCEL_FS_8G     2
#define MPU6050_ACCEL_FS_16G    3

/* 陀螺仪满量程（对应 GYRO_CONFIG 的 FS_SEL[1:0]） */
#define MPU6050_GYRO_FS_250DPS  0
#define MPU6050_GYRO_FS_500DPS  1
#define MPU6050_GYRO_FS_1000DPS 2
#define MPU6050_GYRO_FS_2000DPS 3

typedef struct {
    float ax, ay, az;   /* 加速度，单位 g */
    float gx, gy, gz;   /* 角速度，单位 °/s (dps) */
    float temp_c;       /* 芯片温度，单位 °C（仅供趋势参考，非高精度） */
} mpu6050_data_t;

/* 自己持有总线时用的 I2C 端口。默认 I2C_NUM_0：ESP32-S3 有两个 I2C 控制器，
 * SHT30 已经占了 I2C_NUM_1，复用它必然返回 ESP_ERR_INVALID_STATE。 */
#ifndef MPU6050_BUS_PORT
#define MPU6050_BUS_PORT        I2C_NUM_0
#endif

/* 建一条由本驱动持有的 I2C 总线（幂等），供 mpu6050_init(NULL, ...) 使用。
 *
 * 用哪个 I2C 端口由编译期宏 MPU6050_BUS_PORT 决定，默认 I2C_NUM_0
 * —— 不占用 SHT30 的 I2C_NUM_1，两个传感器各用一对引脚，互不干扰。
 * 想改端口就在工程的编译选项里定义 MPU6050_BUS_PORT（或者干脆不调本函数，
 * 换成上面说的"借用别人的总线"用法）。 */
esp_err_t mpu6050_bus_init(int sda_gpio, int scl_gpio);

/* 把 MPU6050 挂到一条 I2C 总线上（幂等）。
 *
 * bus : 总线句柄。传 NULL 表示"用本驱动自己持有的总线"，即必须先调过
 *       mpu6050_bus_init()；传非 NULL 则是借用调用方的总线，驱动不销毁它。
 * addr: MPU6050_ADDR_AD0_LOW 或 _HIGH。
 *
 * 只做 add_device + 唤醒 + 配置量程，绝不调用 i2c_del_master_bus()。 */
esp_err_t mpu6050_init(i2c_master_bus_handle_t bus, uint8_t addr);

/* 移除设备（幂等）。总线本身不动。 */
esp_err_t mpu6050_deinit(void);

/* 移除设备并销毁 mpu6050_bus_init() 建的那条总线（幂等）。
 * 借用外部总线的用法下，它等价于 mpu6050_deinit()，不会碰别人的总线。 */
esp_err_t mpu6050_bus_deinit(void);

/* 是否已初始化（句柄是否仍在）。读失败不会让它变成 false，
 * 只有 mpu6050_deinit() 才会。 */
bool mpu6050_ready(void);

/* 读一次全部数据，成功返回 true 并填充 out（out 可为 NULL 表示只探测）。
 * 首次失败会尝试重新唤醒并重试一次，之后按实测结果返回。 */
bool mpu6050_read(mpu6050_data_t *out);

/* 单独配置量程；不调用则用 init 时写入的默认值（±2g / ±250dps）。 */
esp_err_t mpu6050_set_accel_range(uint8_t range);
esp_err_t mpu6050_set_gyro_range(uint8_t range);

/* 合加速度与 1g 的偏差，单位 mg（1 mg = 0.001 g）。
 *
 * 静止平放时合加速度约为 1g（重力），偏差接近 0；一旦被敲击、挪动或晃动，
 * 合加速度会偏离 1g，绝对值就是这个偏差。驱动**只算数值不做判断** ——
 * 阈值多少、要连续几次、多久冷却，全部由上层决定。
 * d 为 NULL 时返回 0。 */
float mpu6050_accel_deviation_mg(const mpu6050_data_t *d);

#ifdef __cplusplus
}
#endif
