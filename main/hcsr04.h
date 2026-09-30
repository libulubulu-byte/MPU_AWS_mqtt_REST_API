/* hcsr04.h - HC-SR04 超声波测距驱动
 *
 * 移植说明：本文件与 hcsr04.c 只依赖 ESP-IDF（driver/gpio.h、esp_timer.h），
 * 不 include 任何本项目头文件、不读 Kconfig 宏。TRIG/ECHO 引脚由调用方传入。
 *
 * ⚠️ 硬件要求：HC-SR04 的 ECHO 是 **5V 输出**，ESP32-S3 的 IO 不耐 5V。
 * 必须用分压（例如 1k 上 / 2k 下，得到 3.3V）或电平转换后再接芯片，
 * 否则长期使用会打坏 IO。驱动不会替你保护这个。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化。trig_gpio 为输出触发脚，echo_gpio 为输入回波脚。
 *
 * echo 引脚内部**下拉**：没有模块时读到的电平必然为 0，可以据此判断
 * "模块没接"，而不是让它悬空乱跳造成假回波。 */
esp_err_t hcsr04_init(int trig_gpio, int echo_gpio);

/* 是否已成功初始化。 */
bool hcsr04_ready(void);

/* 测一次距离，成功返回 true 并把厘米数写进 *cm。
 *
 * timeout_us：等待回波上升沿的最长时间。声速按约 343m/s（20°C）算，
 * 即 1cm 往返约 58us，所以 30000us 对应约 5m —— 超过这个距离的物体
 * 会返回 false（不是返回一个大数），调用方应上报 null 而不是 0。
 *
 * 本函数**阻塞**，最长阻塞时间就是 timeout_us（加上极短的触发脉冲），
 * 因此不要在中断或高优先级实时任务里调用。 */
bool hcsr04_measure_cm(float *cm, uint32_t timeout_us);

/* 移除（幂等）：引脚复位释放。 */
void hcsr04_deinit(void);

#ifdef __cplusplus
}
#endif
