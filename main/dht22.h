/* dht22.h - DHT22 / AM2302 温湿度传感器驱动（单总线）
 *
 * 移植说明：本文件与 dht22.c 只依赖 ESP-IDF，不 include 任何本项目头文件、
 * 不读 Kconfig 宏。引脚由调用方通过参数传入，拷走即可用。
 *
 * 时序特点：单总线靠"高电平持续时间"区分 0 和 1（约 26-28us vs 70us），
 * 对中断延迟敏感。驱动在读数期间会临时屏蔽中断（约 5ms），因此**不要**
 * 在中断里或高优先级实时任务里调用。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化。gpio_num 为数据引脚。
 *
 * 3 脚的 DHT22 模块自带上拉电阻，无需外接；4 脚裸传感器需要外接
 * 4.7k~10k 上拉到 3V3。本驱动同时打开内部上拉，对模块而言是双保险，
 * 对裸传感器而言仍不足以替代外部电阻（内部上拉偏弱，长线尤其明显）。 */
esp_err_t dht22_init(int gpio_num);

/* 是否已成功初始化。 */
bool dht22_ready(void);

/* 读一次温湿度，成功返回 true 并填充输出（可传 NULL 表示不取该项）。
 *
 * ⚠️ 器件限制：两次读取之间至少间隔 2 秒（官方数据手册要求），否则
 * 读数不可靠甚至读不到。本驱动内部做了节流 —— 距上次读取不足 2 秒
 * 时直接返回 false 并打一条限频日志，不会真的去读总线。
 *
 * 失败常见原因：上拉电阻缺失、线太长（>20cm 建议缩短）、供电不足。
 * 驱动对一次失败会自动重试一次（DHT22 偶发校验失败很正常）。 */
bool dht22_read(float *temp_c, float *hum_pct);

/* 移除（幂等）：把引脚恢复为输入并释放占用。 */
void dht22_deinit(void);

#ifdef __cplusplus
}
#endif
