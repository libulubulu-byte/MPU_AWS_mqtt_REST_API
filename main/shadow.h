/* shadow.h - AWS IoT Device Shadow 闭环（desired -> 应用 -> reported）
 *
 * 闭环的定义：云端改 desired，设备把指令**真正应用**到硬件，然后回读实际
 * 状态写回 reported。当 reported == desired 时 Shadow 服务不再下发 delta，
 * 闭环收敛。反过来设备侧的任何本地变化（闹钟响了、灯被改了）也通过
 * reported 同步到云端 —— 双向都通才叫闭环，只上报不接收指令不算。
 *
 * 主题（Thing 名从 aws_iot 取，即配网/宏里的 Thing）：
 *   $aws/things/<T>/shadow/update           发布 reported / 清除 desired
 *   $aws/things/<T>/shadow/update/delta     订阅 —— 指令入口
 *   $aws/things/<T>/shadow/update/accepted  订阅 —— 自己的上报被接受
 *   $aws/things/<T>/shadow/update/rejected  订阅 —— 被拒（版本冲突等）
 *   $aws/things/<T>/shadow/update/documents 订阅 —— 每次更新后的完整文档
 *   $aws/things/<T>/shadow/get              发布 —— 主动拉取一次
 *
 * ⚠️ 这些主题**不以 Thing 名开头**（是 $aws/things/<T>/...），所以 AWS IoT
 * 策略里放行 "<T>/..." 的那条**匹配不到它们**，必须额外放行 shadow 主题。
 * 漏配的症状极具误导性：MQTT 连得上、日志不报错、delta 收不到、
 * reported 也没有回执（被策略静默拒绝）。
 *
 * 防"delta 死循环"：
 *   desired 里出现的每个键，reported 必须能回一个相等的值，否则 Shadow
 *   服务会认为没达成、无限重发 delta。所以：
 *     lamp              -> reported 回 "ON"/"OFF"
 *     report_interval_s -> reported 原值回显
 *     alarm_set         -> 应用后把 desired 里的这个键置 null 清掉
 *   reported 里那些"只属于设备"的键（temperature/uptime/alarm...）绝不能
 *   出现在 desired 里，否则同样会循环。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动：注册到 aws_iot 的连接/消息回调，并起周期上报任务。
 * 必须在 aws_iot_start() 之后调用（需要 Thing 名）。
 * 若调用时 MQTT 已连上，会立刻做一次"订阅 + 补报"。 */
esp_err_t shadow_start(void);

/* 本地状态变了（闹钟响了、灯被改了），置脏 -> 尽快上报一次。
 * 由 alarm 的变化回调驱动，避免轮询。 */
void shadow_mark_dirty(void);

/* 立刻上报一次完整 reported（不等周期）。 */
void shadow_publish_now(void);

/* 最近一次指令处理结果（"applied" / "rejected:..."）。 */
const char *shadow_last_cmd_result(void);

/* 当前生效的周期上报间隔（秒）。 */
int shadow_report_interval_s(void);

/* 【测试用】把一段 delta JSON 直接喂给内部处理器，跳过 MQTT。
 *
 * 用途：没有云端控制台权限时，也能在本地验证"解析 desired -> 应用 ->
 * 回读 -> 上报 -> 清 desired"整条链路（走的是和真实 delta **完全相同**的
 * 代码路径，只有传输层被绕过）。
 *
 * 它**不能替代**云端验证：真实的 delta 由 Shadow 服务根据 desired≠reported
 * 计算并下发，还要经过 IoT 策略授权。所以最终验收仍要在控制台的
 * MQTT test client 里发一次 desired。 */
void shadow_inject_delta(const char *json);

#ifdef __cplusplus
}
#endif
