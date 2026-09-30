/* time_sync.h - 系统时间：SNTP 校时 + 断电持久化 + 漂移观测
 *
 * 为什么需要这个模块：
 *   ESP32-S3 的 RTC 计时器在芯片供电期间一直走，深度睡眠也不中断，但
 *   **彻底断电就归零回 1970**。而离线闹钟必须知道"现在几点"，所以时间
 *   要落盘，上电先恢复、联网再校准。
 *
 * 三档时间来源，语义必须分清（报文体和日志里都会带）：
 *   TIME_SRC_NONE : 从没同步过，系统时间停在 1970 —— 闹钟引擎不工作
 *   TIME_SRC_NVS  : 从上电前存的值恢复。**走时真实但不保证准**：
 *                   掉电期间设备没在计时，误差 = 掉电时长 + RTC 漂移
 *   TIME_SRC_SNTP : 已被 NTP 校准，可信
 *
 * 精度说明（本工程用的是片内 RC 慢时钟，见 sdkconfig 的
 * CONFIG_RTC_CLK_SRC_INT_RC=y）：启动时会用 40MHz 晶振校准一次，但
 * 温漂是主要误差源，典型量级每天数十秒。所以 SNTP 的周期性回补
 * （CONFIG_LWIP_SNTP_UPDATE_DELAY）不是可选项，是精度的兜底。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIME_SRC_NONE = 0,
    TIME_SRC_NVS,
    TIME_SRC_SNTP,
} time_src_t;

/* 恢复 NVS 里的时间并设置时区。**不发起任何网络请求**，可在 WiFi 之前调用。
 * 之后系统时间立刻可用于对比（但 synced 仍为 false）。 */
esp_err_t time_sync_init(void);

/* 启动 SNTP 客户端（幂等，可反复调用）。应在拿到 IP 之后调用；
 * 离线时调用也无害 —— 解析不到服务器会自己重试，不影响本地走时。 */
esp_err_t time_sync_start_sntp(void);

/* 起后台任务：1Hz 检查是否需要把时间落盘，并按 APP_TIME_LOG_S 打印
 * "TIME ..." 一行（这行是离线漂移测量的原始数据）。 */
esp_err_t time_sync_start_tasks(void);

/* 当前 epoch 秒（等价 time(NULL)，仅为语义清晰） */
time_t time_sync_now(void);

/* 时间是否已被 NTP 校准过 */
bool time_sync_is_synced(void);

/* 时间是否足够可信到可以跑闹钟（排除 1970） */
bool time_sync_is_valid(void);

time_src_t time_sync_src(void);
const char *time_sync_src_str(void);

/* 立刻把当前时间写回 NVS（强制）。 */
esp_err_t time_sync_save_now(void);

/* 手动设定时间（测试用，例如离线场景下模拟一个已知时间）。 */
esp_err_t time_sync_set_epoch(time_t epoch, time_src_t src);

/* 把当前本地时间格式化成 ISO8601 写进 buf，返回 buf。 */
const char *time_sync_iso(time_t epoch, char *buf, size_t sz);

#ifdef __cplusplus
}
#endif
