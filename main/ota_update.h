/* ota_update.h - 上电版本检查 + OTA 升级
 *
 * 流程（app_main 里在连上 WiFi 之后、启动上报之前调用）：
 *   1. 取本机版本 APP_FW_VERSION（app_version.h，来自 version.cmake）；
 *   2. GET APP_OTA_CHECK_URL 拿版本清单 JSON；
 *   3. 服务器版本 **高于** 本机 -> HTTPS OTA 下载并校验 -> 置启动分区 -> 重启；
 *      相等 -> 跳过（已是最新）；更低 -> 跳过（**不做降级**）。
 *
 * 清单格式（就是最小的一个 JSON，便于用任意静态服务器托管）：
 *   {"version":"1.0.1","url":"https://example.com/fw/aws_mqtt.bin"}
 * 也可以给 version 带前缀（"v1.0.1"）或加 "notes" 之类的额外字段，
 * 解析只认 version / url 两个键，比较时只看数字部分。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 开机检查版本，必要时升级（升级成功会重启，不会返回）。
 *
 * 返回值只在"没有升级"时有意义：
 *   ESP_OK          - 已是最新 / 服务器版本更低（应用层不需要为此报错）
 *   ESP_ERR_INVALID_STATE - 没有配置 URL、或未开启开机检查（功能关闭）
 *   其它            - 检查失败（网络/JSON/HTTP 错误），已打印日志
 */
esp_err_t ota_check_and_update_on_boot(void);

#ifdef __cplusplus
}
#endif
