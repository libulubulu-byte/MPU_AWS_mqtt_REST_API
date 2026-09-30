/* pir.h - PIR 人体红外传感器驱动
 *
 * 移植说明：本文件与 pir.c 只依赖 ESP-IDF（driver/gpio.h），不 include
 * 任何本项目头文件、不读 Kconfig 宏。引脚由调用方传入。
 *
 * 这是个"薄"驱动：PIR 模块自己完成模拟前端和阈值判断，输出一个数字电平，
 * 芯片侧只需要读 GPIO。所以本驱动不做任何去抖/保持逻辑 —— 那些取决于
 * 使用场景（保持多久、要不要锁存），属于应用层的决策，放在上层更好。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化。gpio_num 为模块 OUT 引脚。
 *
 * 输入**下拉**：PIR 模块未接或未稳定时读到低电平（= 无人），比悬空乱跳
 * 安全。多数模块的输出是 3.3V 兼容的，若是 5V 输出请先分压。 */
esp_err_t pir_init(int gpio_num);

/* 是否已成功初始化。 */
bool pir_ready(void);

/* 读当前电平：true = 检出人体活动。
 * 注意这是瞬时电平，模块自身有 2-3s 的再触发窗口，两次读到 true 之间
 * 可能有短暂 false（人还在走动但恰好落在窗口间隙），去抖请在上层做。 */
bool pir_read(void);

/* 移除（幂等）：引脚复位释放。 */
void pir_deinit(void);

#ifdef __cplusplus
}
#endif
