/* aws_iot.h - AWS IoT Core 连接层（MQTT over TLS 双向认证）
 *
 * 这一层只负责"把连接管好、把消息收发出去"，**不含任何业务语义**：
 *   - 连接生命周期、自动重连、TLS/SNI/ALPN 细节都在 aws_iot.c；
 *   - 什么主题、发什么 JSON、收到消息怎么解析，交给上层。
 *
 * 上层通过回调挂进来（见 aws_iot_add_conn_cb / aws_iot_add_msg_cb）：
 *   shadow.c   —— Device Shadow 闭环（desired/delta/reported）
 *   aws_iot.c  —— 内置的 lamp/set 兼容通道（默认关闭）
 *
 * 这样拆的原因：Shadow 需要"连上后立刻订阅+补报"，而上报的字段（时间、
 * 闹钟、温度…）只有业务模块知道。把业务塞进 aws_iot.c 会让这个文件
 * 既懂 MQTT 又懂闹钟，谁都改不动。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#include "app_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 最多注册多少个回调（当前只有 shadow 一个，留余量） */
#define AWS_IOT_MAX_CONN_CBS 4
#define AWS_IOT_MAX_MSG_CBS  4

/* 连接建立后的回调。**注册时若已经连上，会被立刻调用一次** ——
 * 否则先连后注册的模块（比如 Shadow 在 aws_iot_start 之后才启动）
 * 会永远等不到这次"已连接"事件。
 * 回调里可以订阅主题、发布状态；不要做阻塞操作（运行在 MQTT 事件上下文）。 */
typedef void (*aws_iot_conn_cb_t)(void);

/* 收到消息的回调。返回 true 表示"我已处理"，事件分发就此停止。
 * topic 不是以 '\0' 结尾的字符串，长度是 topic_len，比较时务必带上长度。
 * data 同理（不是 C 字符串），长度 data_len。 */
typedef bool (*aws_iot_msg_cb_t)(const char *topic, int topic_len,
                                 const char *data, int data_len);

/* 创建 MQTT 客户端并启动（非阻塞，内部自动重连）。
 * 证书缺失时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t aws_iot_start(const app_cfg_t *cfg);

/* 是否已连上 broker。离线时为 false —— 上层据此避免无意义的发布。 */
bool aws_iot_is_connected(void);

/* Thing 名（Client ID，也是 Shadow 主题里的那段）。启动前返回空串。 */
const char *aws_iot_thing(void);

/* 发布。qos 建议 1（Shadow 要求）；retain 恒为 0 —— AWS IoT Core 默认
 * 不支持保留消息（要单独开 Retain messages），发 retain 会被直接丢弃。
 * 返回 msg_id，<0 表示失败（未连接、参数错等）。 */
int aws_iot_publish(const char *topic, const char *payload, int qos);

/* 订阅。返回 msg_id，<0 表示失败。 */
int aws_iot_subscribe(const char *topic, int qos);

esp_err_t aws_iot_add_conn_cb(aws_iot_conn_cb_t cb);
esp_err_t aws_iot_add_msg_cb(aws_iot_msg_cb_t cb);

#ifdef __cplusplus
}
#endif
