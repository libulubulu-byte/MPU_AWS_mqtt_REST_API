/* app_version.h - 固件版本号（编译期）+ 版本比较（上电 OTA 判断高低用）
 *
 * 版本号的唯一来源是工程根目录的 version.cmake（改那一行即可发版），
 * main/CMakeLists.txt 把它转成 APP_FW_VERSION_MAJOR/_MINOR/_PATCH
 * 三个编译期宏传进来；app_config.h 里只是 #ifndef 兜底。
 * app_config.h 不该再被当成改版本的地方。
 *   version.cmake 里的 "1.0.0"  ->  APP_FW_VERSION  "1.0.0"
 *
 * 三处地方共用这一个宏，避免"串口打印一个版本、上报给服务器另一个版本"：
 *   - 串口启动日志（app_main.c）
 *   - REST 上报 / AWS state 主题里的 "version" 字段（rest_api.c / aws_iot.c）
 *   - OTA 上电版本比较（ota_update.c，只有服务器版本**更高**才升级）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"   /* APP_FW_VERSION_MAJOR / _MINOR / _PATCH（通常是 CMake 传入的） */

#ifdef __cplusplus
extern "C" {
#endif

/* 两级展开，保证宏先被替换成数字再转字符串 */
#define APP_STR_(x) #x
#define APP_STR(x)  APP_STR_(x)

/* 注意：这里必须是纯粹的相邻字符串字面量拼接，不能加外层括号，
 * 否则 "Firmware " APP_FW_VERSION 就不是字面量拼接而是函数调用了。 */
#define APP_FW_VERSION  APP_STR(APP_FW_VERSION_MAJOR) "." \
                        APP_STR(APP_FW_VERSION_MINOR) "." \
                        APP_STR(APP_FW_VERSION_PATCH)

/* 解析 "v1.2.3" / "1.2.3" / "1.2" / "1" 为数值。
 * 返回 false 表示字符串里没有任何数字。前缀（v、release- 等）直接跳过，
 * 只有数字和 '.' 有意义。 */
bool app_version_parse(const char *s, int *major, int *minor, int *patch);

/* cur 与 other 比较：a>b 返回 1，a<b 返回 -1，相等（数值部分）返回 0。 */
int app_version_cmp(const char *a, const char *b);

/* 便捷封装：candidate 是否比 current 新（严格更高，相等/更低都返回 false）。 */
bool app_version_is_newer(const char *candidate, const char *current);

#ifdef __cplusplus
}
#endif
