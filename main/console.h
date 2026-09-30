/* console.h - 串口调试台（读 stdin，跑在独立任务里）
 *
 * 为什么需要它：这台设备没有屏幕也没有按键，闹钟配置的唯一正常入口是云端
 * Device Shadow 的 desired。可"离线闹钟"这个需求的验收场景恰恰是**断网**，
 * 那时候云端指令根本下不来 —— 没有本地入口就无法证明闹钟真的在离线工作，
 * 只能证明云端下发能用。
 *
 * 所以这里做一个最小命令行：设置/查看闹钟、强制设定时间、立即试响。
 * 它只**读** stdin，不往串口写协议数据，不会干扰配网时打印的
 * "ESP32S3_SN:..." 那一行。
 *
 * 用法：串口工具（idf.py monitor 或任意串口助手）敲命令后回车。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 起命令行任务（幂等）。 */
esp_err_t console_start(void);

#ifdef __cplusplus
}
#endif
