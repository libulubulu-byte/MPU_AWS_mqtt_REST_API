/* rest_api.c - 把温湿度 POST 给自己的 REST 服务器，并轮询灯命令
 *
 * 和 aws_iot.c 相比的取舍：
 *   - 不需要证书、不需要 SNI/ALPN，服务器可以是最简单的一个 Flask/Node；
 *   - 没有服务器主动下发（设备是客户端），灯命令只能自己轮询；
 *   - 默认走明文 http://：局域网内调试够用，也不会踩自签证书的坑。
 *     需要 HTTPS 时把 app_config.h 的 APP_REST_USE_TLS 置 1，并把服务器
 *     CA 填进 APP_REST_CA_PEM（留空 = 用内置根证书包，自签证书会失败）。
 *
 * 服务器地址 / 端口 / 路径 / 轮询周期全部来自 main/app_config.h 的宏，
 * 改那些宏后重新编译烧录即可 —— 配网页只管 WiFi，不参与这里。
 *
 * 状态机：s_connected 由每次请求的结果维护 —— 成功置 true，连续失败
 * 到阈值置 false。URL 前缀（http://host:port）在启动时拼一次，
 * 运行中路径由常量决定，不需要每次请求都格式化。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>    /* strcasecmp */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"        /* esp_read_mac / ESP_MAC_WIFI_STA */
#include "sdkconfig.h"

#include "rest_api.h"
#include "app_cfg.h"
#include "app_version.h"
#include "device_id.h"
#include "sht30.h"
#include "lamp.h"
#include "wifi_net.h"
#include "env_sensors.h"

static const char *TAG = "rest_api";

/* 服务器地址前缀，例如 "http://192.168.0.101:8080"（不含路径） */
static char s_base[160];
static char s_report_path[CFG_REST_PATH_LEN];
static char s_cmd_path[CFG_REST_PATH_LEN];
static bool s_use_tls;

/* 设备 ID 在**本文件里持有一份拷贝**。
 * 早先这里存的是 rest_api_start(cfg) 里 cfg->thing_name 的指针，而调用方
 * app_main() 传的是自己栈上的 app_cfg_t —— app_main 返回后那块栈被复用，
 * 指针就悬垂了，上报出去的 "device" 变成空串。现在改为拷进自己的缓冲。 */
static char s_device[48];

/* 服务器 CA 直接取 app_config.h 的宏。空串时 s_ca_pem 长度为 1，
 * 后续用 s_ca_pem[0] 判断"有没有填 CA"依然安全。 */
#define REST_CA_PEM  APP_REST_CA_PEM
static char s_ca_pem[sizeof(REST_CA_PEM)];

#define REPORT_INTERVAL_MS  (APP_REPORT_INTERVAL_S * 1000)
#define POLL_INTERVAL_MS    (APP_REST_POLL_MS)
#define FAILS_BEFORE_OFFLINE 3

static bool s_connected = false;
static int  s_fail_count = 0;
static TaskHandle_t s_task = NULL;

/* 事件组：传感器任务用 rest_api_request_report() 置位，rest_task 等它。
 *
 * 为什么用事件组而不是任务通知：将来若还要加"配置变了""阈值变了"
 * 之类的事件，位图可以一起等，不用把唤醒逻辑改成一团。 */
static EventGroupHandle_t s_evt = NULL;
#define REST_EVT_REPORT   BIT0

/* ------------------------------------------------------------------ */
/* 设备 ID                                                             */
/* ------------------------------------------------------------------ */
/* 实现已抽到 device_id.c，和 AWS Shadow 分支共用同一份 ——
 * 两个后端上报的 "device" 字段必须完全一致，否则服务器侧会把
 * 同一台设备当成两台。 */

/* ------------------------------------------------------------------ */
/* 灯命令解析                                                          */
/* ------------------------------------------------------------------ */

/* 服务器只回一个很小的 JSON。为避免为它引入 cJSON 依赖，这里用"找键 +
 * 取引号里的值"的方式解析；值里前后可能有空格，所以做了修剪。
 * 支持的键：lamp / cmd / value / state，第一个命中的有效值生效。 */
static const char *json_find_value(const char *json, const char *key,
                                   char *out, size_t out_sz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (!p) {
        return NULL;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return NULL;
    }
    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return NULL;      /* 只接受字符串形式的 ON/OFF，"true" 之类不当命令用 */
    }
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_sz) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return out;
}

static bool json_lamp_command(const char *json, bool *on)
{
    static const char *keys[] = { "lamp", "cmd", "value", "state" };
    char v[16];

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!json_find_value(json, keys[i], v, sizeof(v))) {
            continue;
        }
        if (strcasecmp(v, "ON") == 0 || strcmp(v, "1") == 0) {
            *on = true;
            return true;
        }
        if (strcasecmp(v, "OFF") == 0 || strcmp(v, "0") == 0) {
            *on = false;
            return true;
        }
        /* "none" / "keep" / 空串：明确表示"没有命令"，本轮不动作 */
        if (strcasecmp(v, "none") == 0 || strcasecmp(v, "keep") == 0 ||
            v[0] == '\0') {
            return false;
        }
        ESP_LOGW(TAG, "unknown %s value \"%s\" (use ON/OFF)", keys[i], v);
        return false;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* HTTP 基础                                                           */
/* ------------------------------------------------------------------ */

static void fill_http_cfg(esp_http_client_config_t *hc, const char *url,
                          int timeout_ms)
{
    memset(hc, 0, sizeof(*hc));
    hc->url = url;
    hc->timeout_ms = timeout_ms;
    hc->keep_alive_enable = false;   /* 服务器/中间设备可能随时关连接，别复用 */
    if (s_use_tls) {
        if (s_ca_pem[0]) {
            hc->cert_pem = s_ca_pem;
        } else {
            /* 没填 CA 就没有任何校验依据 —— 明确说明这是调试用法，
             * 而不是静默地"跳过校验但看起来像在走 HTTPS" */
            ESP_LOGW(TAG, "REST over HTTPS without a CA - using the built-in "
                          "root bundle (self-signed servers will fail; "
                          "fill APP_REST_CA_PEM)");
            hc->crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 上报 / 轮询                                                         */
/* ------------------------------------------------------------------ */

static bool post_state(void)
{
    float temp = 0.0f, hum = 0.0f;
    if (!sht30_read(&temp, &hum)) {
        ESP_LOGW(TAG, "SHT30 read failed");
        return false;
    }

    /* 环境传感器快照。一次性取出来，避免在拼 JSON 时反复加锁。 */
    env_snapshot_t env;
    env_get_snapshot(&env);

    /* 传感器读不到就发 null，**绝不填 0**：
     * 0°C / 0%RH / 0cm 都是"看起来正常但其实是假的"数据，比缺失更糟 ——
     * 云端会把它当成真值画进曲线、参与告警判断。 */
    char dht_t[16], dht_h[16], dist[16];
    if (env.dht_ok) {
        snprintf(dht_t, sizeof(dht_t), "%.1f", env.dht_temp_c);
        snprintf(dht_h, sizeof(dht_h), "%.1f", env.dht_hum_pct);
    } else {
        snprintf(dht_t, sizeof(dht_t), "null");
        snprintf(dht_h, sizeof(dht_h), "null");
    }
    if (env.dist_ok) {
        snprintf(dist, sizeof(dist), "%.1f", env.distance_cm);
    } else {
        snprintf(dist, sizeof(dist), "null");
    }

    /* payload 随传感器增多而变长。缓冲区按最坏情况给够：
     * 设备 ID 47 + 版本 15 + 两组浮点 + 告警名 12 + RSSI/uptime 约 40，
     * 再加键名和标点，320 足够；给 384 留余量避免
     * -Werror=format-truncation 在数值极端值时报错。 */
    char payload[384];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"version\":\"%s\",\"temperature\":%.1f,"
             "\"humidity\":%.1f,\"temp_dht\":%s,\"hum_dht\":%s,"
             "\"distance_cm\":%s,\"pir\":%s,\"presence\":%s,"
             "\"vibration\":%lu,\"accel_dev_mg\":%.0f,\"alert\":\"%s\","
             "\"rssi\":%d,\"uptime\":%lld,\"lamp\":\"%s\"}",
             s_device, APP_FW_VERSION, temp, hum,
             dht_t, dht_h, dist,
             env.pir_ok ? (env.presence ? "true" : "false") : "null",
             env.pir_ok ? (env.presence ? "true" : "false") : "null",
             (unsigned long)env.vib_count, env.accel_dev_mg,
             env_alert_name(env.alerts),
             net_sta_rssi(),
             (long long)(esp_timer_get_time() / 1000000),
             lamp_get() ? "ON" : "OFF");

    /* 缓冲区要能装下"base + 最长路径 + 余量"：246 与 s_base 的 160 对齐，
     * 太小会被 -Werror=format-truncation 拦住（base 最长 160，路径最长 95） */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base, s_report_path);

    esp_http_client_config_t hc;
    fill_http_cfg(&hc, url, 8000);

    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        ESP_LOGE(TAG, "http client init failed");
        return false;
    }
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int len = esp_http_client_get_content_length(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "POST %s -> HTTP %d", url, status);
        return false;
    }
    ESP_LOGI(TAG, "state -> %s (HTTP %d, %d bytes)", payload, status, len);
    return true;
}

static bool poll_command(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base, s_cmd_path);

    char body[192];
    /* 兜底置空：某些服务器会在 200 里返回空 body */
    memset(body, 0, sizeof(body));

    esp_http_client_config_t hc;
    fill_http_cfg(&hc, url, 5000);

    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return false;
    }
    esp_http_client_set_method(c, HTTP_METHOD_GET);
    esp_http_client_set_header(c, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s open failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return false;
    }

    (void)esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);

    /* 204 = 服务器明确说"没有命令"，正常返回 */
    if (status == 204) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return true;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return false;
    }

    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = esp_http_client_read(c, body + total, sizeof(body) - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    body[total] = '\0';

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    /* 命令端点能应答，说明服务器是活的 —— 借此把在线状态拉回来。
     * 不加这一步的话，s_connected 只有 POST 成功才会翻 true，而 POST
     * 是 10s 一次：服务器重启后即便 /lampcmd 立刻恢复，UI 上的"在线"
     * 也要等下一次上报才更新。这里让两条路径都能证明"服务器活着"。 */
    s_fail_count = 0;
    if (!s_connected) {
        ESP_LOGI(TAG, "REST command endpoint reachable -> back online");
        s_connected = true;
    }

    bool on;
    if (json_lamp_command(body, &on)) {
        /* 先通知应用层，再动灯：应用层据此决定要不要给 PIR 上锁。
         * 顺序反过来的话，PIR 任务可能在我们 set 完灯、还没上锁的
         * 那一瞬间又把灯点着。 */
        env_notify_lamp_command(on);
        if (on != lamp_get()) {
            lamp_set(on);
            ESP_LOGI(TAG, "lamp command \"%s\" -> lamp %s",
                     on ? "ON" : "OFF", on ? "ON" : "OFF");
            /* 立刻把新状态报回去，服务器 UI 不用等下一个上报周期 */
            post_state();
        } else {
            ESP_LOGI(TAG, "lamp command \"%s\" - already %s, nothing to do",
                     on ? "ON" : "OFF", on ? "ON" : "OFF");
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 任务                                                                */
/* ------------------------------------------------------------------ */

/* 发一次上报并维护在线/离线状态机。返回是否成功。 */
static bool do_report(void)
{
    if (post_state()) {
        s_fail_count = 0;
        if (!s_connected) {
            ESP_LOGI(TAG, "REST backend reachable (%s)", s_base);
        }
        s_connected = true;
        return true;
    }

    if (++s_fail_count >= FAILS_BEFORE_OFFLINE) {
        if (s_connected) {
            ESP_LOGW(TAG, "REST backend unreachable, still retrying");
        }
        s_connected = false;
    }
    return false;
}

static void rest_task(void *arg)
{
    (void)arg;
    int64_t next_report_us = 0;      /* 0 = 立刻上报一次（上线先报） */

    for (;;) {
        /* 睡到"下次周期上报"或"有人请求立即上报"中先到的那个。
         *
         * 用事件组而不是纯 vTaskDelay 的原因：传感器任务（震动、PIR 翻转、
         * 告警翻转）需要立即补报一次。若这里只是 delay，它们要么等到下个
         * 周期（震动事件就失去意义），要么自己另开一条 HTTP 路径（两个
         * 任务并发 POST 会互相抢 socket）。事件组让单线程上报和即时性兼得。 */
        int64_t now = esp_timer_get_time();
        int64_t remain_us = next_report_us - now;
        TickType_t wait = (remain_us <= 0) ? 0 : pdMS_TO_TICKS(remain_us / 1000);

        EventBits_t bits = 0;
        if (wait > 0) {
            bits = xEventGroupWaitBits(s_evt, REST_EVT_REPORT, pdTRUE, pdFALSE, wait);
        }

        bool forced = (bits & REST_EVT_REPORT) != 0;
        bool due = (esp_timer_get_time() >= next_report_us);

        if (due || forced) {
            /* 周期到点就重置周期计时；纯事件触发不重置，避免高频震动
             * 把周期上报一直往后推（"上报饥饿"）。 */
            if (due) {
                next_report_us = esp_timer_get_time() +
                                 (int64_t)REPORT_INTERVAL_MS * 1000;
            }
            if (forced && !due) {
                ESP_LOGD(TAG, "event-driven report requested");
            }
            do_report();
        }

        /* 轮询命令：每 POLL_INTERVAL_MS 一次。
         *
         * ⚠️ 这里**不能**加 `if (s_connected)` 门控（曾经有过，已去掉）。
         * s_connected 只由 POST /report 的结果维护，而灯命令走的是
         * GET /lampcmd —— 两个不同的端点。门控会造成一个很隐蔽的失效：
         * 只要上报先连续失败 3 次（服务器刚重启、上报路径 500、网络抖动），
         * s_connected 就变 false，而它**只有在下次 POST 成功时才会翻回来**。
         * 于是即使灯命令端点一直好着，设备也彻底不再去取命令 ——
         * 表现为"云端点开关灯毫无反应"，重启设备才恢复。
         *
         * 现在每次都轮询：GET 失败会走下面的失败分支并打日志。
         * 代价只是服务器不可达时每次多等一个超时（POLL_INTERVAL_MS 默认
         * 1000ms，见 app_config.h），比"灯永远控不了"划算得多。
         * 附带好处：这条 GET 的结果也能把 s_connected 拉回 true，
         * UI 的在线状态不必干等下一次 10s 上报。 */
        poll_command();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void rest_api_request_report(void)
{
    if (s_evt) {
        xEventGroupSetBits(s_evt, REST_EVT_REPORT);
    }
}

esp_err_t rest_api_start(const app_cfg_t *cfg)
{
    if (!cfg->rest_host[0]) {
        ESP_LOGE(TAG, "REST host is empty (set APP_REST_HOST)");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task) {
        return ESP_OK;                /* 幂等：重复调用不重复建任务 */
    }

    s_use_tls = cfg->rest_use_tls;
    /* sizeof(REST_CA_PEM) 就是为了配这个缓冲区大小：空宏时大小为 1，
     * 拷贝结果为空串，后续 s_ca_pem[0] 的判断依然安全 */
    snprintf(s_ca_pem, sizeof(s_ca_pem), "%s", REST_CA_PEM);
    snprintf(s_report_path, sizeof(s_report_path), "%s", cfg->rest_path);
    snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", cfg->rest_cmd_path);
    snprintf(s_base, sizeof(s_base), "%s://%s:%d",
             s_use_tls ? "https" : "http", cfg->rest_host, cfg->rest_port);

    /* 事件组必须在建任务之前就绪：传感器任务可能在 rest_task 起来之前
     * 就调用 rest_api_request_report()，若届时 s_evt 还是 NULL，
     * 那次请求会被静默丢弃。这里先创建，保证时序安全。 */
    if (!s_evt) {
        s_evt = xEventGroupCreate();
        if (!s_evt) {
            ESP_LOGE(TAG, "event group create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 设备 ID：APP_DEVICE_ID 留空时用芯片 MAC 拼成 "esp32s3_aabbccddeeff"。
     * 结果拷进本文件自己的缓冲 —— 不依赖调用方 cfg 的生命周期。 */
    device_id_build(s_device, sizeof(s_device));

    if (xTaskCreate(rest_task, "rest_api", 6144, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "REST backend start: device=%s -> %s%s "
                  "(report %ds, poll %dms, tls=%d)",
             s_device, s_base, s_report_path, APP_REPORT_INTERVAL_S,
             POLL_INTERVAL_MS, (int)s_use_tls);
    return ESP_OK;
}

void rest_api_stop(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    s_connected = false;
}
