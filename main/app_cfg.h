/* app_cfg.h - 运行配置：WiFi 走 NVS，其余全是编译期宏
 *
 * 分工：
 *   WiFi（ssid/pass）  <- SoftAP 网页配网写入 NVS，断电不丢。
 *                         没有配置时设备自动进配网模式。
 *   REST 服务器参数、后端选择 <- main/app_config.h 的宏，改代码重编译
 *   AWS 端点 / Thing 名 / 证书 <- main/aws_certs.h 的宏
 *
 * 也就是说配网页**只填 WiFi**，服务器地址这类东西不会出现在网页上。
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#include "app_config.h"  /* APP_REST_HOST / APP_BACKEND 等编译期宏 */
#include "aws_certs.h"   /* AWS_IOT_ENDPOINT / _PORT / _THING 默认值 */

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_SSID_LEN    33
#define CFG_PASS_LEN    65
#define CFG_REST_HOST_LEN 96
#define CFG_REST_PATH_LEN 96

/* 编译期默认值：宏在 aws_certs.h，跟证书放一起。
 * 长度 +1 留给 snprintf 的结尾 '\0'，用 sizeof 推导，改宏不用改这里。 */
#define CFG_AWS_EP_LEN  (sizeof(AWS_IOT_ENDPOINT))
#define CFG_THING_LEN   (sizeof(AWS_IOT_THING))

/* 上报通道：AWS = MQTT over TLS 连 IoT Core；REST = 自己的 HTTP 服务器。
 * 两个模块内部用的是同类互斥的资源（都是走到 server 的 TCP/TLS 连接），
 * 因此运行时只允许选一个。 */
typedef enum {
    BACKEND_REST = 0,   /* 默认：HTTP POST/GET，无需证书 */
    BACKEND_AWS  = 1,   /* MQTT over TLS 8883 连 AWS IoT Core */
} backend_t;

/* 把后端枚举转成短名字，日志里用（"REST" / "AWS"） */
const char *backend_name(backend_t b);

typedef struct {
    char ssid[CFG_SSID_LEN];              /* WiFi SSID                        */
    char pass[CFG_PASS_LEN];              /* WiFi 密码（空 = 开放网络）        */

    char aws_endpoint[CFG_AWS_EP_LEN];    /* AWS IoT 端点域名（不要带 https://）*/
    int  aws_port;                        /* 8883 = MQTT over TLS             */
    char thing_name[CFG_THING_LEN];       /* Thing 名，同时用作 MQTT Client ID */

    backend_t backend;                    /* 当前生效的上报通道（存 NVS）      */

    char rest_host[CFG_REST_HOST_LEN];    /* REST 服务器域名/IP（不带 http://）*/
    int  rest_port;                       /* REST 服务器端口，默认 8080        */
    char rest_path[CFG_REST_PATH_LEN];    /* 状态上报路径，默认 /api/v1/report */
    char rest_cmd_path[CFG_REST_PATH_LEN];/* 灯命令轮询路径，默认 /api/v1/cmd  */
    bool rest_use_tls;                    /* true = https://，false = http://  */
    char rest_ca_pem[CFG_REST_PATH_LEN];  /* TLS 用的 CA，空 = 不校验（调试）  */
} app_cfg_t;

/* 初始化 NVS（幂等）。 */
esp_err_t cfg_init(void);

/* 读取配置到 out。
 * WiFi 来自 NVS，其余字段一律来自编译期宏（app_config.h / aws_certs.h）；
 * 每次调用都重新从宏赋值，保证改了头文件重烧后立即生效。 */
void cfg_load(app_cfg_t *out);

/* 保存 WiFi 到 NVS（其余编译期项不落盘）。 */
esp_err_t cfg_save(const app_cfg_t *cfg);

/* 清除配置（进入配网模式 / 长按重配网时使用）。 */
esp_err_t cfg_erase(void);

#ifdef __cplusplus
}
#endif
