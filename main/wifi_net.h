/* wifi_net.h - WiFi 管理：SoftAP(配网) / STA(连接) 公共接口 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 netif / 事件循环 / WiFi 驱动及事件处理（幂等）。 */
esp_err_t net_common_init(void);

/* 开启配网热点（返回后设备 IP 为 192.168.4.1）。 */
esp_err_t net_ap_start(const char *ssid, const char *pass);

/* 以 STA 连接路由器并等待拿到 IP，最多 timeout_s 秒。 */
esp_err_t net_sta_connect(const char *ssid, const char *pass, int timeout_s);

/* 当前是否已联网（拿到 IP）。 */
bool net_sta_is_connected(void);

/* 当前连接 AP 的信号强度 dBm（0 表示不可用）。 */
int net_sta_rssi(void);

/* 【测试用】模拟断网：true = 主动断开并**停止一切自动重连**，
 * false = 恢复正常重连。
 *
 * 存在的理由：验收要求"断开网络后验证本地 RTC 仍走时、闹钟仍触发"。
 * 拔路由器/搬设备都能做到，但既不精确也不可重复（还要顺带影响其它设备）。
 * 用软件切断 WiFi 能达到同样效果 —— 设备侧看到的都是
 * WIFI_EVENT_STA_DISCONNECTED + 没有 IP，闹钟引擎完全不知道差别。 */
void net_sta_suppress(bool suppress);

/* 是否处于"被强制离线"状态。 */
bool net_sta_is_suppressed(void);

#ifdef __cplusplus
}
#endif
