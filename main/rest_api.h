/* rest_api.h - REST 上报通道（HTTP，无需证书）
 *
 * 和 aws_iot.c 是**同一层的两条互斥通道**：由 app_main 按**编译期**的
 * APP_BACKEND 宏（main/app_config.h）选一个启动，同一时刻只有一个在跑。
 * 注意不是 NVS/运行期选的：cfg_load() 每次都把 backend 从宏重新赋值
 * （app_cfg.c 的 BACKEND_DFLT），换后端必须改宏重新编译烧写。
 *
 * 协议（服务器侧自己实现，路径见 Kconfig）：
 *   上报：POST  <rest_path>       {"device":"ESP32S3","version":"1.0.0", ...}
 *   命令：GET   <rest_cmd_path>   {"lamp":"ON"} / {"lamp":"OFF"} / 204
 *
 * 与 AWS 版的差异：REST 没有服务器推消息的通道，所以灯命令靠**轮询**
 * （APP_REST_POLL_INTERVAL_MS，默认 1s）；上报周期沿用
 * APP_PUBLISH_INTERVAL_S。
 */
#pragma once

#include "esp_err.h"
#include "app_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 REST 上报任务（非阻塞）。cfg->rest_host 为空时返回
 * ESP_ERR_INVALID_ARG。 */
esp_err_t rest_api_start(const app_cfg_t *cfg);

/* 停止上报任务并释放资源（切换后端时用；当前流程只在重启时切换）。 */
void rest_api_stop(void);

/* 请求**立刻**补���一次状态（事件驱动）。
 *
 * 用途：震动、PIR 状态翻转、告警位变化这些"事件"不该等到下一个周期
 * 才被云端看到。传感器任务调用本函数后立即返回，由 rest_task 单线程
 * 去发 HTTP POST。
 *
 * 为什么不让调用方自己发 HTTP：POST 会阻塞数秒，而且两个任务并发
 * POST 会互相抢网络和缓冲区。这里只置一个事件位唤醒上报任务，天然
 * 保证"同一时刻只有一个上报在途"。
 *
 * 未启动（或启动失败）时是空操作，安全可调。 */
void rest_api_request_report(void);

#ifdef __cplusplus
}
#endif
