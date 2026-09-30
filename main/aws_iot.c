/* aws_iot.c - AWS IoT Core 连接层（MQTT over TLS 8883 双向认证）
 *
 * AWS IoT Core 的连接要点（和普通 Mosquitto 差别很大）：
 *   1. 8883 端口是 TLS，必须校验证书：设备要带根 CA + 设备证书 + 私钥，
 *      broker 会用 SNI(Server Name Indication) 决定出示哪张服务器证书，
 *      所以 .broker.verification.use_global_ca_store 之外一定要设
 *      `.broker.address.hostname` 或直接用完整 mqtts:// URI 让 esp-tls 带上 SNI；
 *   2. AWS 还要求 ALPN 协商 "x-amzn-mqtt-ca"（ALPN 是硬性要求，不协商会直接被断），
 *      通过 `.broker.verification.alpn_protos` 指定；
 *   3. Client ID 必须全局唯一，否则两台设备会互相顶掉（AWS 后连的踢掉先连的）。
 *      这里用 Thing 名，并把它作为主题前缀的一部分。
 *
 * 证书来自编译期宏（main/aws_certs.h），不走 NVS：
 *   换证书要改头文件重新编译烧写，好处是私钥不必经过配网热点上的明文 HTTP。
 *
 * 本文件只做连接层，业务在上层：
 *   shadow.c —— Device Shadow（$aws/things/<thing>/shadow/...）；
 *   本文件里的 lamp/set 与 <base>/state 是**兼容通道**，只在
 *   APP_SHADOW_ENABLE == 0 时才编译进来（同一颗灯被两条通道控制必然打架）。
 *
 * 关于证书有效期：sdkconfig 里 CONFIG_MBEDTLS_HAVE_TIME_DATE 默认**未开启**，
 * 也就是 mbedTLS 不校验证书的 notBefore/notAfter —— 这正是设备在没有
 * RTC 电池、开机时间还是 1970 时也能完成 TLS 握手的原因。
 * 现在有了 SNTP，可以把它打开（更安全，但要求开机必须先校时成功），
 * 取舍见 README「证书有效期校验」一节。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "aws_iot.h"
#include "aws_certs.h"
#include "app_config.h"
#include "device_id.h"
#include "app_version.h"
#include "sht30.h"
#include "lamp.h"
#include "wifi_net.h"

static const char *TAG = "aws_iot";

#define BASE_TOPIC          CONFIG_APP_AWS_BASE_TOPIC
#define STATE_TOPIC         BASE_TOPIC "/state"
#define LAMP_STATE_TOPIC    BASE_TOPIC "/lamp/state"
#define LAMP_SET_TOPIC      BASE_TOPIC "/lamp/set"

/* AWS IoT 要求 ALPN 必须是 x-amzn-mqtt-ca（443 端口走 WebSocket 时才用别的） */
#define AWS_ALPN_PROTO      "x-amzn-mqtt-ca"

#define PUBLISH_INTERVAL_MS (APP_REPORT_INTERVAL_S * 1000)

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;

/* MQTT 客户端在整个生命周期内引用这些缓冲区，必须是静态的。 */
static char s_uri[256];
/* Thing 名（配置为空时用 aws_certs.h 里的 AWS_IOT_THING）：Shadow 主题里
 * 要嵌它，所以这里留一份副本，不能只存在于 startup 栈上。 */
static char s_thing[CFG_THING_LEN];
static char s_device[48];
/* esp-tls 的 alpn_protos 是非 const 指针数组，必须可写且常驻 */
static const char *s_alpn[2] = { AWS_ALPN_PROTO, NULL };

/* 回调注册表 */
static aws_iot_conn_cb_t s_conn_cbs[AWS_IOT_MAX_CONN_CBS];
static int s_conn_cb_n = 0;
static aws_iot_msg_cb_t s_msg_cbs[AWS_IOT_MAX_MSG_CBS];
static int s_msg_cb_n = 0;

/* ------------------------------------------------------------------ */
/* 分片重组                                                            */
/* ------------------------------------------------------------------ */
/* 为什么必须做：Shadow 的 $aws/things/<T>/shadow/update/documents 会把
 * 完整文档发回来，**带上每个字段的 metadata 时间戳**，实测 3~6 KB，远超
 * 单次接收缓冲，esp-mqtt 只能分成多个 MQTT_EVENT_DATA 上报。
 *
 * esp-mqtt 的分片语义（核对过 mqtt_client.c:1096-1126）：
 *   首片 current_data_offset == 0，**带 topic**；
 *   后续片 current_data_offset 递增，topic = NULL / topic_len = 0；
 *   收齐的判据是 current_data_offset + data_len == total_data_len。
 *
 * 初版代码在本处直接"发现分片就丢弃"，结果 documents 全被扔了 ——
 * 日志只剩一堆 "fragmented MQTT message ignored"，而闭环判据恰好要靠
 * documents。所以这里必须真的拼起来。
 *
 * 缓冲区按 AWS 对 Shadow 文档的 8 KB 上限留了余量；超限就丢弃并告警，
 * 宁可少一条日志也不要越界写。 */
#define RX_ASM_MAX   10240
#define RX_TOPIC_MAX 192

static char s_rx_asm[RX_ASM_MAX];
static int  s_rx_len = 0;
static char s_rx_topic[RX_TOPIC_MAX];
static int  s_rx_topic_len = 0;

/* ------------------------------------------------------------------ */
/* 回调注册                                                            */
/* ------------------------------------------------------------------ */

esp_err_t aws_iot_add_conn_cb(aws_iot_conn_cb_t cb)
{
    if (!cb || s_conn_cb_n >= AWS_IOT_MAX_CONN_CBS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_conn_cbs[s_conn_cb_n++] = cb;

    /* 关键：注册时若已连上，立刻补一次 —— 上层模块通常在 aws_iot_start()
     * 之后才启动，错过 MQTT_EVENT_CONNECTED 会导致它永不订阅/永不补报。 */
    if (s_connected) {
        ESP_LOGI(TAG, "already connected - invoking conn callback immediately");
        cb();
    }
    return ESP_OK;
}

esp_err_t aws_iot_add_msg_cb(aws_iot_msg_cb_t cb)
{
    if (!cb || s_msg_cb_n >= AWS_IOT_MAX_MSG_CBS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_msg_cbs[s_msg_cb_n++] = cb;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 发布 / 订阅                                                         */
/* ------------------------------------------------------------------ */

bool aws_iot_is_connected(void)
{
    return s_connected;
}

const char *aws_iot_thing(void)
{
    return s_thing;
}

int aws_iot_publish(const char *topic, const char *payload, int qos)
{
    if (!s_client) {
        return -1;
    }
    /* retain 恒为 0：AWS IoT Core 的保留消息要额外开 Retain messages 才有，
     * 默认不支持，带 retain 发了也是白发。 */
    int id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, 0);
    if (id < 0) {
        ESP_LOGW(TAG, "publish to %s failed (msg_id=%d)", topic, id);
    }
    return id;
}

int aws_iot_subscribe(const char *topic, int qos)
{
    if (!s_client) {
        return -1;
    }
    int id = esp_mqtt_client_subscribe(s_client, topic, qos);
    if (id < 0) {
        ESP_LOGW(TAG, "subscribe to %s failed (msg_id=%d)", topic, id);
    }
    return id;
}

/* ------------------------------------------------------------------ */
/* 兼容通道：裸主题（APP_SHADOW_ENABLE == 0 时才编译）                    */
/* ------------------------------------------------------------------ */

#if APP_SHADOW_ENABLE == 0

/* 逐字节比较，避免为 2~3 字节的 payload 分配缓冲区 */
static bool payload_on(const char *d, int n)
{
    if (n == 2) {
        return (d[0] == 'O' || d[0] == 'o') && (d[1] == 'N' || d[1] == 'n');
    }
    return n == 1 && d[0] == '1';
}

static bool payload_off(const char *d, int n)
{
    if (n == 3) {
        return (d[0] == 'O' || d[0] == 'o')
            && (d[1] == 'F' || d[1] == 'f')
            && (d[2] == 'F' || d[2] == 'f');
    }
    return n == 1 && d[0] == '0';
}

static void publish_legacy_state(void)
{
    float temp = 0.0f, hum = 0.0f;
    if (!sht30_read(&temp, &hum)) {
        ESP_LOGW(TAG, "SHT30 read failed");
        return;
    }

    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"version\":\"%s\",\"temperature\":%.1f,"
             "\"humidity\":%.1f,\"rssi\":%d,\"uptime\":%lld}",
             s_device, APP_FW_VERSION, temp, hum, net_sta_rssi(),
             (long long)(esp_timer_get_time() / 1000000));
    aws_iot_publish(STATE_TOPIC, payload, 0);
    ESP_LOGI(TAG, "state -> %s", payload);
}

static void publish_legacy_lamp_state(void)
{
    aws_iot_publish(LAMP_STATE_TOPIC, lamp_get() ? "ON" : "OFF", 1);
    ESP_LOGI(TAG, "lamp state -> %s", lamp_get() ? "ON" : "OFF");
}

static void on_legacy_lamp_command(const char *data, int len)
{
    if (payload_on(data, len)) {
        lamp_set(true);
    } else if (payload_off(data, len)) {
        lamp_set(false);
    } else {
        ESP_LOGW(TAG, "unknown lamp command: \"%.*s\" (use ON/OFF)", len, data);
        return;
    }
    publish_legacy_lamp_state();
}

static void legacy_publish_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_connected) {
            publish_legacy_state();
        }
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL_MS));
    }
}

#endif /* APP_SHADOW_ENABLE == 0 */

/* ------------------------------------------------------------------ */
/* 消息分发                                                            */
/* ------------------------------------------------------------------ */

/* 完整的（已拼好的）报文在这里进入上层。
 * 定义位置有意放在兼容通道之后：APP_SHADOW_ENABLE=0 时要调用它的
 * on_legacy_lamp_command()，放在前面会变成隐式声明。 */
static void dispatch_message(const char *topic, int topic_len,
                             const char *data, int data_len)
{
    for (int i = 0; i < s_msg_cb_n; i++) {
        if (s_msg_cbs[i](topic, topic_len, data, data_len)) {
            return;                    /* 已被某个模块认领 */
        }
    }
#if APP_SHADOW_ENABLE == 0
    if (topic_len == (int)strlen(LAMP_SET_TOPIC) &&
        strncmp(topic, LAMP_SET_TOPIC, topic_len) == 0) {
        on_legacy_lamp_command(data, data_len);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* MQTT 事件                                                           */
/* ------------------------------------------------------------------ */

/* ⚠️ 回调里只做"拷贝 + 入队"，真正的解析交给独立任务。
 *
 * 为什么不能让回调干重活 —— 两条独立的路径都指向同一个后果（断连）：
 *
 * 1) QoS1 回执被拖晚。PUBACK 是在**回调返回之后**才拼、才发出去的
 *    （mqtt_client.c:1386-1418，deliver_publish() 里同步走完我们的事件
 *    派发，再 mqtt_msg_puback() + esp_mqtt_write()）。回调里只要耗掉
 *    几百毫秒，回执就迟到；broker 侧等不到就关连接。
 *
 * 2) 更隐蔽的是分片路径。当报文大于 .buffer.size 时，deliver_publish()
 *    在派发完事件后会**在 mqtt 任务上同步阻塞**再读一截
 *    （mqtt_client.c:1117 的 esp_transport_read(..., network_timeout_ms)）。
 *    也就是说：**回调慢 --(同步派发)--> 用户回调占用 mqtt 任务 -->
 *    分片读晚** 三者是同一条链上的。
 *
 * 日志长相（区分这两条路径的指纹）：
 *   路径 1：/error 事件里 esp_tls_last_esp_err / sock_errno 都是 0，
 *          因为底层没报错，是 mqtt_process_receive 主动 return ESP_FAIL；
 *   路径 2：会先出现
 *            E esp-tls-mbedtls: read error :-0x7200
 *            E transport_base: esp-tls-mbedtls: read error, errno=119
 *            E mqtt_client: ... transport_read() error: errno=119 (EINPROGRESS)
 *          再是
 *            E mqtt_client: mqtt_process_receive: mqtt_message_receive() returned -2
 *   两条最后都收敛成同一行：
 *     E aws_iot: transport error: tls=0x0, errno=0
 *     W aws_iot: disconnected from AWS IoT, will auto-reconnect
 *   —— 所以光看 aws_iot 这行去查 TLS 是死路，必须往上翻 mqtt_client /
 *   transport_base 的那几行。
 *
 * ⚠️ 但要说清楚：**以上两条都不是"隔十几秒断一次"那个 bug 的原因**。
 * 那个 bug 的真正根因在 TLS 记录层 —— CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
 * 配得小于对端单条记录（当时生效值是 6144，而 AWS 的
 * shadow/update/documents 解出来约 6248 字节，只超出 100 多字节，
 * 恰好卡在"握手用的 5007 能过、documents 过不去"的缝里），
 * 见 sdkconfig.defaults 里那一节的详细说明。
 *
 * 当时曾误判成"回调慢 + 分片"，于是加了队列和异步任务；这些改动本身是
 * 合理优化（回调确实不该站在收包关键路径上），但**改了之后断连依旧** ——
 * 它们把注意力引开了，绕了好几轮才回到 TLS。
 *
 * 排查这类"周期性断连"的正确顺序（血泪总结）：
 *   1. 先把**周期**量准：断连间隔是否等于某个业务周期？如果随上报间隔
 *      一起变，就一定是"收到应答"触发的，别去查 keepalive / 网络。
 *      （实测：上报间隔 10s -> 存活 11~12s；改成 60s -> 存活 61s。）
 *   2. 看 E 行的**第一条**，而不是最后那条汇总。第一手错误才是根因，
 *      aws_iot 打的 "transport error: tls=0x0, errno=0" 是二次转录，
 *      它把 mbedtls 的具体错误码丢掉了，照着它查必然跑偏。
 *   3. 认错误码：-0x7200 = MBEDTLS_ERR_SSL_INVALID_RECORD（TLS 记录坏了），
 *      -0x7100 = 请求的数据超出记录长度上限。两者都指向记录层配置。
 *   4. errno=119 EINPROGRESS 在这个场景里是**假线索** —— 它是记录解析
 *      失败后 esp-tls 残留的状态，与 connect 无关，不要顺着它去查并发。
 *   5. ⚠️ 最省时间的一步其实是**先确认配置真的生效**：这类问题十有八九
 *      出在"改了配置文件但固件没编进去"。直接查编译产物，别信源文件：
 *          Select-String build\config\sdkconfig.h -Pattern "SSL_IN_CONTENT_LEN"
 *      本例就栽在这里 —— sdkconfig.defaults 里早已是 16384，但根目录
 *      sdkconfig 仍是 6144（defaults 只在 sdkconfig 不存在时才生成它），
 *      于是"这个方向试过了"的判断是错的，白白多绕了几轮。
 *
 * 另外务必确认 CONFIG_LOG_MAXIMUM_LEVEL >= 4，否则 ESP_LOGD 在**编译期**
 * 就被删掉，运行时再怎么 esp_log_level_set() 都是空的（见 app_main.c）。
 */
#define MSGQ_LEN    8
/* ⚠️ 上限必须 >= 最大可能的**单条**报文。documents 实测 6085 字节
 * （不是之前以为的 3.1 KB —— 3100 那个是 update/accepted），按 AWS 的
 * 8 KB 文档上限留余量。抠小了 documents 会被静默丢弃（日志只有一行
 * "message of 6085 bytes dropped"），而闭环判据恰好靠它。
 * 取 RX_ASM_MAX 同量级，两处不会各自漂移。 */
#define MSGQ_MAXSZ  RX_ASM_MAX

typedef struct {
    char  topic[RX_TOPIC_MAX];
    int   len;
    char *data;
} msgq_item_t;

static msgq_item_t s_msgq[MSGQ_LEN];
static volatile int s_msgq_head = 0, s_msgq_tail = 0;
static uint32_t s_msg_drop = 0;

/* 把一条完整报文**拷贝**后入队。队列满就丢最旧的，宁可少打一条日志也
 * 不能让消息积压 —— 积压同样会拖慢 PUBACK。 */
static void enqueue_msg(const char *topic, int topic_len,
                        const char *data, int data_len)
{
    if (data_len <= 0 || data_len > MSGQ_MAXSZ) {
        ESP_LOGW(TAG, "message of %d bytes dropped (%s)", data_len,
                 (data_len > MSGQ_MAXSZ) ? "larger than the queue item" : "empty");
        return;
    }

    int head = s_msgq_head;
    int next = (head + 1) % MSGQ_LEN;

    if (next == s_msgq_tail) {
        /* 满了：把最旧的一条连同它的 data 一起丢掉并前移 tail，
         * 否则 tail 处的 data 指针会被下面覆盖后泄漏。 */
        free(s_msgq[s_msgq_tail].data);
        s_msgq[s_msgq_tail].data = NULL;
        s_msgq_tail = (s_msgq_tail + 1) % MSGQ_LEN;
        s_msg_drop++;
    }

    char *copy = malloc(data_len);
    if (!copy) {
        ESP_LOGE(TAG, "no memory for a %d-byte message - dropped", data_len);
        return;
    }
    memcpy(copy, data, data_len);

    msgq_item_t *it = &s_msgq[head];
    free(it->data);                     /* 上一轮用过的槽，指针已归还 */
    it->data = copy;
    it->len = data_len;
    int tl = (topic && topic_len > 0) ? topic_len : 0;
    if (tl > (int)sizeof(it->topic) - 1) {
        tl = (int)sizeof(it->topic) - 1;
    }
    if (tl > 0) {
        memcpy(it->topic, topic, tl);
    }
    it->topic[tl] = '\0';

    s_msgq_head = next;
}

/* 下游解析任务。这里才是慢活（cJSON、日志、上层的 publish）。 */
static void msg_proc_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_msgq_tail == s_msgq_head) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        msgq_item_t *it = &s_msgq[s_msgq_tail];
        dispatch_message(it->topic, (int)strlen(it->topic), it->data, it->len);
        free(it->data);
        it->data = NULL;
        s_msgq_tail = (s_msgq_tail + 1) % MSGQ_LEN;
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to AWS IoT Core (%s)", s_uri);
        s_connected = true;

#if APP_SHADOW_ENABLE == 0
        /* 兼容模式：先订阅命令，再发状态，避免刚上线时的下发命令被丢掉 */
        aws_iot_subscribe(LAMP_SET_TOPIC, 1);
        publish_legacy_state();
        publish_legacy_lamp_state();
#endif

        /* 通知上层（shadow.c 在这里订阅 delta / 补报 reported）。
         * 顺序很重要：上层必须先订阅再发布，否则上线瞬间的 delta 会丢。 */
        for (int i = 0; i < s_conn_cb_n; i++) {
            s_conn_cbs[i]();
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from AWS IoT, will auto-reconnect");
        s_connected = false;
        /* 断线时把没拼完的分片丢掉：连接重建后不会再有后续片，
         * 留着只会让下一条消息接在半个包后面拼出一个畸形 JSON。 */
        if (s_rx_len > 0) {
            ESP_LOGW(TAG, "dropping %d bytes of an incomplete fragment", s_rx_len);
            s_rx_len = 0;
        }
        break;

    case MQTT_EVENT_DATA: {
        const int total = event->total_data_len;
        const int off   = event->current_data_offset;
        const int len   = event->data_len;

        /* 收到大报文时把尺寸打出来（配合上面的回执超时问题排查用）。
         * 降为 DEBUG：每次上报都会带回 accepted(~3 KB) 与 documents(~6 KB)，
         * 挂 INFO 就是每 10 秒固定两行常驻噪音。 */
        if (total > 1024) {
            ESP_LOGD(TAG, "large inbound message: %d bytes (chunk=%d off=%d)",
                     total, len, off);
        }

        /* 单包：绝大多数消息都走这条（delta 只有几十~几百字节）。
         * 这里**只入队不解析**，让回调立刻返回去发 PUBACK。 */
        if (total <= len) {
            enqueue_msg(event->topic, event->topic_len,
                        event->data, event->data_len);
            break;
        }

        /* 首片：记下长度，并**保存 topic** —— 后续片 esp-mqtt 不再给 topic
         * （mqtt_client.c:1114 把 msg_topic 置 NULL），不存下来就不知道该
         * 把拼好的报文投给哪个主题。 */
        if (off == 0) {
            s_rx_len = 0;
            s_rx_topic_len = (event->topic && event->topic_len > 0)
                           ? event->topic_len : 0;
            if (s_rx_topic_len > RX_TOPIC_MAX - 1) {
                s_rx_topic_len = RX_TOPIC_MAX - 1;
            }
            if (s_rx_topic_len > 0) {
                memcpy(s_rx_topic, event->topic, s_rx_topic_len);
            }
            s_rx_topic[s_rx_topic_len] = '\0';
            ESP_LOGD(TAG, "fragmented message start: %d bytes on %s",
                     total, s_rx_topic);
        }

        if (s_rx_len + len > RX_ASM_MAX) {
            ESP_LOGE(TAG, "fragmented message too large (%d > %d) - dropped; "
                          "check the shadow document size", total, RX_ASM_MAX);
            s_rx_len = 0;
            break;
        }

        memcpy(s_rx_asm + s_rx_len, event->data, len);
        s_rx_len += len;

        /* 收齐了才入队（同样不在回调里解析） */
        if (off + len >= total) {
            ESP_LOGI(TAG, "reassembled %d bytes on %s", s_rx_len, s_rx_topic);
            enqueue_msg(s_rx_topic, s_rx_topic_len, s_rx_asm, s_rx_len);
            s_rx_len = 0;
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        /* AWS 侧证书/策略不对时最常见的就是 TLS 握手失败。
         * 注意：策略缺权限不会报错 —— 只表现为发布/订阅石沉大海，见 README。 */
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error: tls=0x%x, errno=%d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "MQTT error, type=%d", event->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 启动                                                                */
/* ------------------------------------------------------------------ */

esp_err_t aws_iot_start(const app_cfg_t *cfg)
{
    /* AWS 端点只填域名，别带 https:// / mqtts:// */
    const char *ep = cfg->aws_endpoint;
    if (strncmp(ep, "https://", 8) == 0) {
        ep += 8;
    } else if (strncmp(ep, "mqtts://", 8) == 0) {
        ep += 8;
    } else if (strncmp(ep, "mqtt://", 7) == 0) {
        ep += 7;
    }
    /* 端点里误带了端口或路径就截掉，端口由 aws_port 决定 */
    char host[CFG_AWS_EP_LEN];
    snprintf(host, sizeof(host), "%s", ep);
    char *cut = strpbrk(host, ":/");
    if (cut) {
        *cut = '\0';
    }
    if (host[0] == '\0') {
        ESP_LOGE(TAG, "AWS endpoint is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_FALSE(aws_certs_configured(), ESP_ERR_INVALID_STATE, TAG,
                        "certificates not filled in - edit main/aws_certs.h "
                        "(see the comment at the top of that file)");

    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%d", host, cfg->aws_port);
    snprintf(s_thing, sizeof(s_thing), "%s",
             cfg->thing_name[0] ? cfg->thing_name : AWS_IOT_THING);
    /* 设备 ID 与 REST 后端共用同一份实现，保证服务器侧看到的是同一台设备 */
    device_id_build(s_device, sizeof(s_device));

    ESP_LOGI(TAG, "endpoint=%s port=%d thing=%s device=%s alpn=%s fw=%s",
             host, cfg->aws_port, s_thing, s_device, s_alpn[0], APP_FW_VERSION);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = s_uri,
            /* 关键：让 esp-tls 用这个域名做 SNI 和证书 CN 校验 */
            .verification = {
                .certificate = AWS_IOT_CA_PEM,
                .alpn_protos = s_alpn,
            },
        },
        .credentials = {
            .client_id = s_thing,
            .authentication = {
                .certificate = AWS_IOT_CERT_PEM,
                .key = AWS_IOT_KEY_PEM,
            },
        },
        .session = {
            .keepalive = 60,
            .disable_clean_session = false,
        },
        .network = {
            /* 离线场景下设备会长期连不上，5s 一次的重连日志太吵，
             * 而且每次都要走一遍 TCP+TLS，15s 更合理。 */
            .reconnect_timeout_ms = 15000,
            .timeout_ms = 15000,
        },
        .buffer = {
            /* ⚠️⚠️ size 必须 >= 8192，这是整套代码里最要命的一个数字。
             *
             * 实测：documents 报文 **6085 字节**（不是之前以为的 3.1 KB ——
             * 3100 那个是 update/accepted）。AWS 对 Shadow 文档的硬上限是
             * 8 KB，所以接收缓冲必须按 8 KB 留，否则**一定会分片**。
             *
             * 分片为什么会把连接搞断（esp-mqtt 收包路径的连环坑）：
             * deliver_publish() 在报文读满缓冲、还有剩余时，会在
             * **派发完 MQTT_EVENT_DATA 之后立刻同步调用**
             *     esp_transport_read(..., network_timeout_ms)   // mqtt_client.c:1114
             * 去取剩下那截。这行跑在 mqtt 任务上、且用的是**阻塞**超时，
             * 而此刻我们的回调也可能正在跑 —— 两个上下文叠加，只要有一次
             * 读没拿到数据，socket 就被留在 "connection in progress" 状态：
             *     E esp-tls-mbedtls: read error :-0x7200
             *     E transport_base: esp_tls_conn_read error, errno=Connection already in progress
             *     E mqtt_client: ... transport_read() error: errno=119   (EINPROGRESS)
             *     E mqtt_client: mqtt_process_receive: mqtt_message_receive() returned -2
             * 上层 mqtt_process_receive 见 recv < 0 就 return ESP_FAIL，
             * 主循环随即 abort_connection —— 连接就断了。
             *
             * 所以"不超限"才是根治手段：**报文永远单包到达，分片路径压根
             * 不进入**。注意 .buffer.size 与 CONFIG_MQTT_BUFFER_SIZE 是两套
             * 东西：config 里显式写了 size 就以显式值为准（mqtt_client.c:401），
             * Kconfig 那个只在本字段缺省时才兜底。两个都设 8192 是为了
             * 看代码的人不误判。
             *
              * out_size 别为省内存调小：QoS 1 的订阅（delta / accepted /
             * rejected / documents 全是 QoS1）每收一条都要回 PUBACK，而
             * PUBACK 是在**出站缓冲**里现场拼的（mqtt_client.c:1390-1418），
             * 出站缓冲不够会 ESP_LOGE("Publish response ... cannot be
             * created") 并 return ESP_FAIL，同样把连接掐掉。
             *
             * ⚠️ 若把 out_size 降到 4 KB 以下，我们自己的 reported
             * （800~900 字节）会走 **出站分片** 路径 —— esp-mqtt 把整个
             * payload 拷进 outbox（mqtt_client.c:2055 的 remaining_data），
             * 多一份 heap 拷贝，而且 send_publish 用整段超时同步写 socket。
             * 所以这里保持和 .size 一样大：让 1 KB 级的报文永远单包发。 */
            .size = 8192,
            .out_size = 8192,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_FAIL, TAG, "mqtt client init");

    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                       mqtt_event_handler, NULL),
                        TAG, "register mqtt event");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "mqtt client start");

    /* 下游解析任务：把慢活从 MQTT 事件回调里搬出来，保证 PUBACK 及时发出。
     * 栈给 8 KB —— log_documents() 要在 6 KB 报文上建 cJSON 树再打印一遍
     * （cJSON 对每个节点都要 malloc，深度递归也是吃栈的）。 */
    if (xTaskCreate(msg_proc_task, "msg_proc", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "msg_proc task");
    }

#if APP_SHADOW_ENABLE == 0
    if (xTaskCreate(legacy_publish_task, "aws_publish", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "publish task");
    }
    ESP_LOGI(TAG, "AWS IoT start: uri=%s base=%s interval=%ds (legacy topics)",
             s_uri, BASE_TOPIC, APP_REPORT_INTERVAL_S);
#else
    ESP_LOGI(TAG, "AWS IoT start: uri=%s thing=%s (Device Shadow mode)",
             s_uri, s_thing);
#endif
    return ESP_OK;
}
