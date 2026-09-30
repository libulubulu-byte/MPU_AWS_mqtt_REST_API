/* sht30.h - SHT30 温湿度传感器驱动（I2C）
 *
 * 硬件：SHT30，I2C 7 位地址 0x44，SDA/SCL 引脚见 menuconfig 默认 GPIO12/13。
 * 依赖：ESP-IDF v5.x 的 i2c_master 新驱动。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 I2C 总线并添加 SHT30 设备（幂等）。 */
esp_err_t sht30_init(void);

/* 释放设备与 I2C 总线（幂等）。 */
esp_err_t sht30_deinit(void);

/* 立即读一次温湿度。成功返回 true 并填充 *temp_c / *hum_pct（可传 NULL）。 */
bool sht30_read(float *temp_c, float *hum_pct);

/* 取出 I2C 总线句柄，供同一总线上的其它设备复用。
 *
 * ⚠️ 目前**没有**任何调用者：MPU6050 改成挂自己的总线（I2C_NUM_0，
 * GPIO15/16，见 mpu6050_bus_init()）之后就不再借这条总线了。
 * 保留它是为了给"以后往 SHT30 那对引脚上再挂一个小器件"留口子 ——
 * 同一条 I2C 总线不能初始化两次，谁先初始化谁持有句柄，后挂的设备
 * 只能通过本函数借用，自己再 i2c_new_master_bus() 会返回
 * ESP_ERR_INVALID_STATE。若确认不再需要，可连同实现一起删掉。
 *
 * 必须在 sht30_init() 之后调用；未初始化时返回 NULL。 */
i2c_master_bus_handle_t sht30_get_bus(void);

#ifdef __cplusplus
}
#endif
