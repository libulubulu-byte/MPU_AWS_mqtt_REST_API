/* shadow.c - Device Shadow 闭环实现（详见 shadow.h）
 *
 * 报文约定（完整例子）：
 *
 * reported（设备 -> 云端，周期或变化时发）：
 * {
 *   "state": { "reported": {
 *     "device": "esp32s3_b43a45a6b4c4", "fw": "1.0.1",
 *     "lamp": "ON", "temperature": 25.3, "humidity": 60.1, "rssi": -58,
 *     "uptime": 123,
 *     "report_interval_s": 10,
 *     "time":  { "epoch": 1757836800, "iso": "2026-09-14T09:20:00",
 *                "src": "sntp", "synced": true, "tz": "CST-8" },
 *     "alarm": { "state": "idle", "next_fire": 1757838600,
 *                "next_fire_iso": "...", "last_fire": 0, "ringing_left_s": 0 },
 *     "alarms": [ { "id": 1, "enable": true, "hour": 7, "minute": 30,
 *                   "days": [1,2,3,4,5], "duration_s": 10 } ],
 *     "cmd_result": "applied", "cmd_seq": 3
 *   } }
 * }
 *
 * desired（云端 -> 设备，任意子集）：
 * { "state": { "desired": {
 *     "lamp": "OFF",
 *     "report_interval_s": 30,
 *     "alarm_set": { "id": 1, "enable": true, "hour": 6, "minute": 45,
 *                    "days": [1,2,3,4,5], "duration_s": 15 }
 * } } }
 *
 * 为什么要"回读"而不是直接报"我以为的值"：
 *   lamp_set() 在指示器（闹钟闪红）期间不会立刻改变显示颜色，但**逻辑状态**
 *   已经变了；而温度这类值必须真的从传感器读到才算数。回读保证 reported
 *   描述的是设备的真实状态，而不是一厢情愿。
 */
#include <string.h>
#include <stdio.h>
#include <strings.h>    /* strcasecmp */
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include "shadow.h"
#include "aws_iot.h"
#include "app_config.h"
#include "device_id.h"
#include "app_version.h"
#include "sht30.h"
#include "lamp.h"
#include "alarm.h"
#include "time_sync.h"
#include "wifi_net.h"

static const char *TAG = "shadow";

#define TOPIC_LEN 192
#define PAY_LEN   1280

static char s_t_update[TOPIC_LEN];
static char s_t_update_delta[TOPIC_LEN];
static char s_t_update_acc[TOPIC_LEN];
static char s_t_update_rej[TOPIC_LEN];
static char s_t_update_docs[TOPIC_LEN];
static char s_t_get[TOPIC_LEN];
static char s_t_get_acc[TOPIC_LEN];

static char s_device[48];

/* 报文体缓冲区是 static 的（1KB+ 放栈上会顶爆 MQTT 事件上下文的任务栈），
 * 因此构建 + 发布必须串行 —— 两处可能同时发（周期任务、MQTT 事件回调）。 */
static char s_pay[PAY_LEN];
static SemaphoreHandle_t s_pay_lock = NULL;

static volatile bool s_dirty = true;
static volatile int  s_interval_s = APP_SHADOW_REPORT_S;
static char s_cmd_result[48] = "none";
static uint32_t s_cmd_seq = 0;
static bool s_ver_retry = false;      /* rejected 后是否已重试过无版本发布 */

/* ------------------------------------------------------------------ */
/* 报文拼装工具                                                        */
/* ------------------------------------------------------------------ */

/* 追加格式化，返回新的偏移（始终保证以 '\0' 结尾、不越界）。 */
static size_t appendf(char *buf, size_t sz, size_t off, const char *fmt, ...)
{
    if (off >= sz) {
        return sz - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sz - off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return off;
    }
    size_t no = off + (size_t)n;
    return (no >= sz) ? sz - 1 : no;
}

/* 把 bit0=周日 的位图转成 [1,2,3,4,5] 这样的数组文本（可读性优先，
 * 不用数字位图 —— 在 MQTT test client 里能直接看懂是验收时很实际的需求）。 */
static size_t append_days(char *buf, size_t sz, size_t off, uint8_t days)
{
    off = appendf(buf, sz, off, "[");
    bool first = true;
    for (int d = 0; d < 7; d++) {
        if (days & (1u << d)) {
            off = appendf(buf, sz, off, "%s%d", first ? "" : ",", d);
            first = false;
        }
    }
    return appendf(buf, sz, off, "]");
}

static size_t append_alarm_status(char *buf, size_t sz, size_t off)
{
    time_t nf = alarm_next_fire();
    time_t lf = alarm_last_fire();
    char nf_iso[32] = "", lf_iso[32] = "";

    if (nf) {
        time_sync_iso(nf, nf_iso, sizeof(nf_iso));
    }
    if (lf) {
        time_sync_iso(lf, lf_iso, sizeof(lf_iso));
    }

    off = appendf(buf, sz, off, "\"alarm\":{\"state\":\"%s\",\"next_fire\":%lld,"
                                "\"next_fire_iso\":\"%s\",\"last_fire\":%lld,"
                                "\"last_fire_iso\":\"%s\",\"ringing_left_s\":%d},",
                  alarm_state_str(), (long long)nf, nf_iso,
                  (long long)lf, lf_iso, alarm_ringing_remaining_s());

    off = appendf(buf, sz, off, "\"alarms\":[");
    for (int i = 0; i < ALARM_SLOTS; i++) {
        const alarm_t *a = alarm_get(i);
        if (!a) {
            continue;
        }
        off = appendf(buf, sz, off, "%s{\"id\":%u,\"enable\":%s,\"hour\":%u,"
                                    "\"minute\":%u,\"days\":",
                      (i == 0) ? "" : ",", (unsigned)a->id,
                      a->enable ? "true" : "false",
                      (unsigned)a->hour, (unsigned)a->minute);
        off = append_days(buf, sz, off, a->days);
        off = appendf(buf, sz, off, ",\"duration_s\":%u}", (unsigned)a->duration_s);
    }
    off = appendf(buf, sz, off, "],");
    return off;
}

/* ------------------------------------------------------------------ */
/* 上报                                                                */
/* ------------------------------------------------------------------ */

/* 构建并发布完整 reported。with_version > 0 时带上 version 字段 ——
 * 这是对 delta 的应答，AWS 用它做乐观并发检查：若期间影子又被改过，
 * 会回 rejected(409)，而不是默默覆盖掉更新的 desired。 */
static void publish_reported(bool with_version, int version)
{
    if (!aws_iot_is_connected()) {
        s_dirty = true;                 /* 连不上就留着，上线后补报 */
        ESP_LOGD(TAG, "reported deferred: MQTT not connected");
        return;
    }
    if (s_pay_lock) {
        xSemaphoreTake(s_pay_lock, portMAX_DELAY);
    }

    float temp = 0.0f, hum = 0.0f;
    bool sensor_ok = sht30_read(&temp, &hum);

    time_t now = time_sync_now();
    char iso[32];
    time_sync_iso(now, iso, sizeof(iso));

    size_t off = 0;
    s_pay[0] = '\0';
    off = appendf(s_pay, sizeof(s_pay), off, "{\"state\":{\"reported\":{");
    off = appendf(s_pay, sizeof(s_pay), off,
                  "\"device\":\"%s\",\"fw\":\"%s\",\"lamp\":\"%s\",",
                  s_device, APP_FW_VERSION, lamp_get() ? "ON" : "OFF");

    if (sensor_ok) {
        off = appendf(s_pay, sizeof(s_pay), off,
                      "\"temperature\":%.1f,\"humidity\":%.1f,", temp, hum);
    } else {
        /* 读失败就不要报旧值/0 值 —— 那是在撒谎。报 null 让云端知道"这次没有" */
        off = appendf(s_pay, sizeof(s_pay), off,
                      "\"temperature\":null,\"humidity\":null,");
    }

    off = appendf(s_pay, sizeof(s_pay), off,
                  "\"rssi\":%d,\"uptime\":%lld,\"report_interval_s\":%d,",
                  net_sta_rssi(), (long long)(esp_timer_get_time() / 1000000),
                  s_interval_s);

    off = appendf(s_pay, sizeof(s_pay), off,
                  "\"time\":{\"epoch\":%lld,\"iso\":\"%s\",\"src\":\"%s\","
                  "\"synced\":%s,\"tz\":\"%s\",\"valid\":%s},",
                  (long long)now, iso, time_sync_src_str(),
                  time_sync_is_synced() ? "true" : "false", APP_TZ,
                  time_sync_is_valid() ? "true" : "false");

    off = append_alarm_status(s_pay, sizeof(s_pay), off);

    off = appendf(s_pay, sizeof(s_pay), off,
                  "\"cmd_result\":\"%s\",\"cmd_seq\":%u",
                  s_cmd_result, (unsigned)s_cmd_seq);

    if (with_version && version > 0) {
        off = appendf(s_pay, sizeof(s_pay), off, ",\"version\":%d", version);
    }
    off = appendf(s_pay, sizeof(s_pay), off, "}}}");

    /* 整包原文只留在 DEBUG：这是每 10s 一条、每条几百字节的常驻输出，
     * 挂 INFO 的话串口基本只剩它。要看"到底报了什么"用下面 documents
     * 里那行摘要（<TAG> reported = T=... H=...），那是从云端回来的
     * 权威副本，比打印本地拼装结果更有说服力（本地拼错了也一样打）。 */
    ESP_LOGD(TAG, "reported -> %zu bytes: %s", off, s_pay);
    /* QoS1：reported 是设备状态的权威来源，丢了会导致云端长期停旧值，
     * 所以要 PUBACK 兜底。注意这会让 AWS 回一份 update/accepted
     * （~3 KB，含完整 shadow metadata），对端会把这条记录和后续控制
     * 报文连着发 —— 接收侧的 TLS 记录上限必须放得下，见
     * sdkconfig.defaults 里 CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN 的说明。 */
    aws_iot_publish(s_t_update, s_pay, 1);

    if (s_pay_lock) {
        xSemaphoreGive(s_pay_lock);
    }
}

/* 应用成功后清掉 desired 里的指令键。不清的话：
 *   - desired 会一直留着过期指令，云端 UI 看起来像"还在等待执行"；
 *   - 设备重启后旧指令会被当成新 delta 再执行一次。
 * 用 null 显式清除是 AWS 规定的用法。 */
static void clear_desired(const char keys[][24], int n)
{
    if (n <= 0) {
        return;
    }
    if (s_pay_lock) {
        xSemaphoreTake(s_pay_lock, portMAX_DELAY);
    }

    size_t off = 0;
    s_pay[0] = '\0';
    off = appendf(s_pay, sizeof(s_pay), off, "{\"state\":{\"desired\":{");
    for (int i = 0; i < n; i++) {
        off = appendf(s_pay, sizeof(s_pay), off, "%s\"%s\":null",
                      (i == 0) ? "" : ",", keys[i]);
    }
    off = appendf(s_pay, sizeof(s_pay), off, "}}}");

    ESP_LOGI(TAG, "clearing desired: %s", s_pay);
    aws_iot_publish(s_t_update, s_pay, 1);

    if (s_pay_lock) {
        xSemaphoreGive(s_pay_lock);
    }
}

/* ------------------------------------------------------------------ */
/* delta 处理                                                          */
/* ------------------------------------------------------------------ */

static void set_result(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_cmd_result, sizeof(s_cmd_result), fmt, ap);
    va_end(ap);
}

/* days: [1,2,3,4,5] -> bitmask（bit0=周日） */
static uint8_t days_from_json(const cJSON *arr)
{
    uint8_t days = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 6) {
            days |= (uint8_t)(1u << it->valueint);
        }
    }
    return days;
}

static int find_slot_by_id(int id)
{
    for (int i = 0; i < ALARM_SLOTS; i++) {
        const alarm_t *a = alarm_get(i);
        if (a && a->id == (uint8_t)id) {
            return i;
        }
    }
    return -1;
}

/* 处理 desired.alarm_set：{id, enable, hour, minute, days[], duration_s} */
static bool apply_alarm_set(const cJSON *o)
{
    int id = 0;
    const cJSON *j = cJSON_GetObjectItem(o, "id");
    if (j && cJSON_IsNumber(j)) {
        id = j->valueint;
    }

    int slot = (id > 0) ? find_slot_by_id(id) : alarm_find_free();
    if (slot < 0) {
        set_result("rejected:no_free_alarm_slot");
        return false;
    }

    const alarm_t *cur = alarm_get(slot);
    alarm_t a = {
        .id = (id > 0) ? (uint8_t)id : cur->id,
        .enable = cur->enable,
        .hour = cur->hour,
        .minute = cur->minute,
        .days = cur->days,
        .duration_s = cur->duration_s,
    };

    /* 局部更新：desired 里给哪个字段就改哪个，没给就保持原值 —— 这样
     * "只把闹钟关掉"这种指令不需要把整条闹钟重新描述一遍。 */
    if ((j = cJSON_GetObjectItem(o, "enable")) && cJSON_IsBool(j)) {
        a.enable = cJSON_IsTrue(j);
    }
    if ((j = cJSON_GetObjectItem(o, "hour")) && cJSON_IsNumber(j)) {
        a.hour = (uint8_t)j->valueint;
    }
    if ((j = cJSON_GetObjectItem(o, "minute")) && cJSON_IsNumber(j)) {
        a.minute = (uint8_t)j->valueint;
    }
    if ((j = cJSON_GetObjectItem(o, "days")) && cJSON_IsArray(j)) {
        a.days = days_from_json(j);
    }
    if ((j = cJSON_GetObjectItem(o, "duration_s")) && cJSON_IsNumber(j)) {
        a.duration_s = (uint16_t)j->valueint;
    }

    if (alarm_set(slot, &a) != ESP_OK) {
        set_result("rejected:alarm_invalid");
        return false;
    }
    return true;
}

static void on_delta(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        set_result("rejected:bad_json");
        s_cmd_seq++;
        ESP_LOGE(TAG, "delta JSON parse failed (%d bytes)", len);
        return;
    }

    int version = 0;
    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (ver && cJSON_IsNumber(ver)) {
        version = ver->valueint;
    }

    const cJSON *state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsObject(state)) {
        set_result("rejected:no_state");
        cJSON_Delete(root);
        s_cmd_seq++;
        return;
    }

    /* ⚠️ 必须再下钻一层到 "desired"。delta 的 payload 长这样：
     *   { "state": { "desired": { "lamp": "ON" } }, "version": 7 }
     * 顶层 state 里装的是 **desired 容器**，不是指令字段本身。
     * 初版直接遍历 state，于是唯一的 key 是 "desired"，每个指令都被判成
     * "unknown_key=desired" 而全部拒绝 —— 而日志里两行看起来都很"合理"：
     *   delta key "desired" not applied (rejected:unknown_key=desired)
     *   delta handled: 0 key(s) applied
     * 这种"解析成功但语义错层"的 bug 不会报错，只会让功能静默失效。
     *
     * 兼容处理：如果 state 里没有 desired（有人手工发扁平结构做调试），
     * 就退回按原样遍历，避免把调试路径堵死。 */
    const cJSON *cmd = cJSON_GetObjectItem(state, "desired");
    if (!cJSON_IsObject(cmd)) {
        cmd = state;
    }

    /* 哪些键成功应用了（用于稍后清 desired），最多 4 个。
     *
     * ⚠️ 必须**拷贝字符串**，不能只存指针：这些 key 是指向 cJSON 树的
     * it->string，而下面 cJSON_Delete(root) 会把整棵树释放掉，之后再用
     * 就是 use-after-free。实测的表现是清 desired 的报文变成
     *   {"state":{"desired":{"D{?D{?<乱码>":null}}}
     * —— 发出去的 JSON 完全不合法，云端静默拒绝，desired 永远清不掉，
     * 于是每轮上报都会再收到同一个 delta，指令反复执行。 */
    char applied_keys[4][24];
    int applied_n = 0;
    bool any_reject = false;

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, cmd) {
        const char *k = it->string ? it->string : "?";
        bool ok = false;

        if (strcmp(k, "lamp") == 0 && cJSON_IsString(it)) {
            if (strcasecmp(it->valuestring, "ON") == 0) {
                lamp_set(true);
                set_result("applied");
                ok = true;
            } else if (strcasecmp(it->valuestring, "OFF") == 0) {
                lamp_set(false);
                set_result("applied");
                ok = true;
            } else {
                set_result("rejected:lamp=%s", it->valuestring);
            }
        } else if (strcmp(k, "report_interval_s") == 0 && cJSON_IsNumber(it)) {
            int v = it->valueint;
            if (v < 5)  v = 5;
            if (v > 3600) v = 3600;
            s_interval_s = v;
            set_result("applied");
            ok = true;
        } else if (strcmp(k, "alarm_set") == 0 && cJSON_IsObject(it)) {
            ok = apply_alarm_set(it);
            if (ok) {
                set_result("applied");
            }
        } else {
            set_result("rejected:unknown_key=%s", k);
        }

        if (ok) {
            if (applied_n < (int)(sizeof(applied_keys) / sizeof(applied_keys[0]))) {
                snprintf(applied_keys[applied_n], sizeof(applied_keys[0]), "%s", k);
                applied_n++;
            }
        } else {
            any_reject = true;
            ESP_LOGW(TAG, "delta key \"%s\" not applied (%s)", k, s_cmd_result);
        }
    }

    s_cmd_seq++;
    cJSON_Delete(root);

    /* 回读 + 上报。带 version 做乐观并发检查。 */
    s_ver_retry = false;
    publish_reported(version > 0, version);

    /* 只清成功应用的那些键：被拒的留着，云端能看出"这条件没生效"。 */
    if (applied_n > 0) {
        clear_desired(applied_keys, applied_n);
    }
    ESP_LOGI(TAG, "delta handled: %d key(s) applied, reject=%d, result=%s",
             applied_n, (int)any_reject, s_cmd_result);
}

/* ------------------------------------------------------------------ */
/* 其他主题                                                            */
/* ------------------------------------------------------------------ */

static bool topic_is(const char *topic, int topic_len, const char *full)
{
    return topic_len == (int)strlen(full) && strncmp(topic, full, topic_len) == 0;
}

/* documents 是每次更新后的**完整影子文档**（desired + reported + metadata），
 * 实测 3~6 KB，全打到串口会刷屏，所以只摘出关键几项：
 *   - desired 原文（很小）：能一眼看出云端还留着哪些没被满足的指令；
 *   - reported 里的 lamp / cmd_result / alarm.state：确认设备真的按指令动作了。
 * 闭环判据就看 desired 是否已消失 —— 比在控制台页面上肉眼比对可靠得多。 */
static void log_documents(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "documents: unparsable (%d bytes)", len);
        return;
    }

    /* ⚠️ documents 的顶层结构**不是** {"state":...}，而是
     *   { "previous": {"state":...,"metadata":...,"version":N},
     *     "current":  {"state":...,"metadata":...,"version":N},
     *     "timestamp": N }
     * 初版按顶层 "state" 解析，永远取到 NULL，于是每次都打印
     * "desired = <none> ==> RECONCILED" —— 一个恒真的假阳性，
     * 恰好是最容易被当成"闭环成功"的证据。实测才发现：
     * 日志说闭环、控制台里 desired 还在。 */
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    const cJSON *prev = cJSON_GetObjectItem(root, "previous");
    const cJSON *cur_state = cur ? cJSON_GetObjectItem(cur, "state") : NULL;
    const cJSON *ver = cur ? cJSON_GetObjectItem(cur, "version") : NULL;
    const cJSON *desired = cur_state ? cJSON_GetObjectItem(cur_state, "desired") : NULL;
    const cJSON *reported = cur_state ? cJSON_GetObjectItem(cur_state, "reported") : NULL;

    if (!cur_state) {
        ESP_LOGW(TAG, "documents: unexpected layout (%d bytes) - "
                      "top-level keys are not previous/current", len);
        cJSON_Delete(root);
        return;
    }

    const cJSON *pver = prev ? cJSON_GetObjectItem(prev, "version") : NULL;
    ESP_LOGI(TAG, "documents v%d (was v%d, %d bytes)",
             (ver && cJSON_IsNumber(ver)) ? ver->valueint : 0,
             (pver && cJSON_IsNumber(pver)) ? pver->valueint : 0, len);

    if (desired) {
        char *s = cJSON_PrintUnformatted(desired);
        ESP_LOGI(TAG, "  desired  = %s", s ? s : "?");
        cJSON_free(s);
    } else {
        ESP_LOGI(TAG, "  desired  = <none>  ==> RECONCILED, closed loop complete");
    }

    if (reported) {
        const cJSON *lamp = cJSON_GetObjectItem(reported, "lamp");
        const cJSON *res = cJSON_GetObjectItem(reported, "cmd_result");
        const cJSON *alm = cJSON_GetObjectItem(reported, "alarm");
        const cJSON *ast = alm ? cJSON_GetObjectItem(alm, "state") : NULL;
        const cJSON *t = cJSON_GetObjectItem(reported, "temperature");
        const cJSON *h = cJSON_GetObjectItem(reported, "humidity");

        /* 温湿度分开格式化：传感器读失败时上报的是 null，直接按 %f 打会
         * 变成 0.0 —— 看起来像"报了 0 度"，把"没读到"误读成"读到了"。
         * 反过来，这行也是"云端到底有没有收到数据"的权威判据：它是从
         * 云端回来的 documents 里摘的，不是本地拼装结果。 */
        char tb[12] = "null", hb[12] = "null";
        if (t && cJSON_IsNumber(t)) {
            snprintf(tb, sizeof(tb), "%.1f", t->valuedouble);
        }
        if (h && cJSON_IsNumber(h)) {
            snprintf(hb, sizeof(hb), "%.1f", h->valuedouble);
        }

        ESP_LOGI(TAG, "  reported = T=%s H=%s lamp=%s cmd_result=%s alarm=%s",
                 tb, hb,
                 (lamp && cJSON_IsString(lamp)) ? lamp->valuestring : "?",
                 (res && cJSON_IsString(res)) ? res->valuestring : "?",
                 (ast && cJSON_IsString(ast)) ? ast->valuestring : "?");
    }
    cJSON_Delete(root);
}

static bool on_message(const char *topic, int topic_len, const char *data, int data_len)
{
    if (topic_is(topic, topic_len, s_t_update_delta)) {
        on_delta(data, data_len);
        return true;
    }
    if (topic_is(topic, topic_len, s_t_update_acc)) {
        cJSON *root = cJSON_ParseWithLength(data, data_len);
        if (root) {
            const cJSON *ver = cJSON_GetObjectItem(root, "version");
            ESP_LOGI(TAG, "update ACCEPTED (shadow version=%d)",
                     ver && cJSON_IsNumber(ver) ? ver->valueint : 0);
            cJSON_Delete(root);
        }
        return true;
    }
    if (topic_is(topic, topic_len, s_t_update_rej)) {
        ESP_LOGE(TAG, "update REJECTED: %.*s", data_len, data);
        /* 最常见的原因是 version 冲突（期间影子被改过）。重试一次
         * 不带 version 的发布 —— 我们的 reported 是设备真实状态，
         * 无条件写回去永远是对的。只重试一次，避免打转。 */
        if (!s_ver_retry) {
            s_ver_retry = true;
            ESP_LOGW(TAG, "retrying reported without version");
            publish_reported(false, 0);
        }
        return true;
    }
    if (topic_is(topic, topic_len, s_t_update_docs)) {
        log_documents(data, data_len);
        return true;
    }
    if (topic_is(topic, topic_len, s_t_get_acc)) {
        cJSON *root = cJSON_ParseWithLength(data, data_len);
        if (root) {
            const cJSON *state = cJSON_GetObjectItem(root, "state");
            const cJSON *rep = state ? cJSON_GetObjectItem(state, "reported") : NULL;
            const cJSON *des = state ? cJSON_GetObjectItem(state, "desired") : NULL;
            ESP_LOGW(TAG, "shadow GET: cloud has reported=%s desired=%s",
                     rep ? "yes" : "none", des ? "yes" : "none");
            cJSON_Delete(root);
        }
        return true;
    }
    return false;
}

/* ⚠️ 这个回调是**在 esp-mqtt 任务的事件循环里同步跑的**，必须立刻返回。
 *
 * 调用栈（mqtt_client.c:1040 / 1580）：
 *     esp_mqtt_task 已持 MQTT_API_LOCK
 *       -> esp_mqtt_dispatch_event()
 *            -> esp_event_post_to(..., portMAX_DELAY)   // 队列长度 = EVENT_QUEUE_SIZE
 *            -> esp_event_loop_run()                    // 同步跑我们的回调
 *                 -> on_connected()  ← 我们在这里
 * 在真正返回之前，esp_mqtt_task 一直卡在 esp_event_loop_run 里面，
 * 既不会去 drain outbox，也不会去读 socket。
 *
 * 之前的写法在这个回调里直接打 4 个 subscribe + 2 个 publish：
 *   - subscribe 会 enqueue 一个 SUBSCRIBE 包；publish 会 enqueue 一个
 *     813 字节的 PUBLISH(等 PUBACK)；
 *   - 这些全排队等着，而队列的主人正卡在等我们返回 —— 自己堵自己；
 *   - 一旦返回，esp_mqtt_task 才一口气把 6 个包冲出去，紧接着又要处理
 *     AWS 回过来的 SUBACK/PUBACK；这段"突发"期间只要 poll_read 出现
 *     一次空轮询，socket 就被留在 connection-in-progress：
 *       E esp-tls-mbedtls: read error :-0x7200
 *       E transport_base: esp_tls_conn_read error, errno=Connection already in progress
 *       E mqtt_client: ... transport_read() error: errno=119  (EINPROGRESS)
 *       E mqtt_client: mqtt_process_receive: mqtt_message_receive() returned -2
 *     -> mqtt_process_receive 返回 ESP_FAIL -> abort_connection -> 断连。
 * 实测症状就是"每次连上后 1.5~2.5 s 必断"，重连周期 24~27 s，极有规律。
 *
 * 所以这里只置一个标志，真正的订阅/补报交给 shadow_task 去做（它有
 * 自己的上下文，不占着 mqtt 事件循环）。 */
static volatile bool s_need_resubscribe = false;

static void on_connected(void)
{
    ESP_LOGI(TAG, "MQTT connected - shadow topics will be (re)subscribed by shadow_task");
    s_need_resubscribe = true;
}

/* 上线后要做的全部 MQTT 动作，统一放在 shadow_task 的上下文里执行。
 * 顺序：先订阅再发布 —— 反过来会丢掉上线瞬间到达的 delta。 */
static void do_subscribe_and_report(void)
{
    aws_iot_subscribe(s_t_update_delta, 1);
    aws_iot_subscribe(s_t_update_acc, 1);
    aws_iot_subscribe(s_t_update_rej, 1);
    aws_iot_subscribe(s_t_update_docs, 1);
    /* get/accepted 也得订阅：下面那个 GET 请求的响应就落在这个主题上。
     * 之前漏了这行，于是请求发出去了、响应没人收，on_message() 里处理
     * get/accepted 的分支成了死代码 —— "设备上报成功但云端仍是旧值"
     * 这个排查手段静默失效，看起来像"功能没生效"，其实是白发了。
     * 位置必须在发布之前：反过来的话响应可能比订阅先到，同样收不到。 */
    aws_iot_subscribe(s_t_get_acc, 1);

    /* 补报：离线期间闹钟响过、灯被改过，云端都不知道。
     * 这一步保证"离线做了什么"一定同步上去。 */
    publish_reported(false, 0);

    /* 再主动 GET 一次，把云端侧的状态打出来便于核对 ——
     * 尤其是"设备上报成功但云端仍是旧值"的情况，GET 能立刻看出来。 */
    aws_iot_publish(s_t_get, "", 1);

    s_dirty = false;
}

/* ------------------------------------------------------------------ */
/* 任务与启动                                                          */
/* ------------------------------------------------------------------ */

static void shadow_task(void *arg)
{
    (void)arg;
    TickType_t next_periodic = xTaskGetTickCount();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!aws_iot_is_connected()) {
            s_dirty = true;              /* 离线期间的状态变化不能丢 */
            continue;
        }

        /* 刚连上：订阅 + 补报。放在这里而不是 on_connected()，
         * 是为了不在 mqtt 事件回调里做 MQTT API 调用（见该函数上方注释）。 */
        if (s_need_resubscribe) {
            s_need_resubscribe = false;
            ESP_LOGI(TAG, "MQTT connected - subscribing shadow topics for thing=%s",
                     aws_iot_thing());
            do_subscribe_and_report();
            next_periodic = xTaskGetTickCount() + pdMS_TO_TICKS(s_interval_s * 1000);
            continue;
        }

        TickType_t now = xTaskGetTickCount();

        if (s_dirty) {
            s_dirty = false;
            publish_reported(false, 0);
            next_periodic = now + pdMS_TO_TICKS(s_interval_s * 1000);
            continue;
        }
        if (now >= next_periodic) {
            next_periodic = now + pdMS_TO_TICKS(s_interval_s * 1000);
            publish_reported(false, 0);
            /* 周期上报是"设备还活着"的唯一心跳。
             * 曾经因为 MQTT 收缓冲不够，documents 报文超限让 esp-mqtt
             * 每隔 10s 断开一次连接，于是这里永远走在
             * "!connected -> s_dirty=true -> continue" 这条分支上，
             * 一条 reported 都发不出去，而日志里只有一行行重连告警。
             * 把 tick 打出来，能一眼看出是"没到周期"还是"连接又断了"。 */
            ESP_LOGD(TAG, "periodic report sent (interval=%ds, tick=%lu)",
                     s_interval_s, (unsigned long)now);
        }
    }
}

esp_err_t shadow_start(void)
{
    const char *thing = aws_iot_thing();
    if (!thing || thing[0] == '\0') {
        ESP_LOGE(TAG, "thing name is empty - call aws_iot_start() first");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_t_update,      sizeof(s_t_update),      "$aws/things/%s/shadow/update", thing);
    snprintf(s_t_update_delta,sizeof(s_t_update_delta),"$aws/things/%s/shadow/update/delta", thing);
    snprintf(s_t_update_acc,  sizeof(s_t_update_acc),  "$aws/things/%s/shadow/update/accepted", thing);
    snprintf(s_t_update_rej,  sizeof(s_t_update_rej),  "$aws/things/%s/shadow/update/rejected", thing);
    snprintf(s_t_update_docs, sizeof(s_t_update_docs), "$aws/things/%s/shadow/update/documents", thing);
    snprintf(s_t_get,         sizeof(s_t_get),         "$aws/things/%s/shadow/get", thing);
    snprintf(s_t_get_acc,     sizeof(s_t_get_acc),     "$aws/things/%s/shadow/get/accepted", thing);

    device_id_build(s_device, sizeof(s_device));

    if (!s_pay_lock) {
        s_pay_lock = xSemaphoreCreateMutex();
        if (!s_pay_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(aws_iot_add_conn_cb(on_connected));
    ESP_ERROR_CHECK(aws_iot_add_msg_cb(on_message));

    /* 栈给 8 KB。documents 报文实测 3100 字节（单包，未分片），
     * log_documents() 要把它整棵解析成 cJSON 树，再调 cJSON_PrintUnformatted()
     * 生成字符串 —— 这两个都是深递归 + 多次 malloc，6 KB 栈在 3 KB 报文下
     * 会溢出。
     *
     * 栈溢出在这个工程里的表现**极具欺骗性**：不是重启，而是
     *   E aws_iot: transport error: tls=0x0, errno=0
     *   W aws_iot: disconnected from AWS IoT, will auto-reconnect
     * 因为消息处理是在 esp-mqtt 自己的任务里跑回调，栈被踩坏后
     * esp-mqtt 任务异常，错被归到"传输错误"上。而 tls=0x0 / errno=0
     * 恰恰说明**底层没有任何错误**，是上层主动断开 —— 这个组合就是
     * 栈溢出的指纹。 */
    if (xTaskCreate(shadow_task, "shadow", 8192, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "shadow start: device=%s thing=%s interval=%ds",
             s_device, thing, s_interval_s);
    ESP_LOGI(TAG, "  update topic: %s", s_t_update);
    ESP_LOGI(TAG, "  NOTE: the AWS policy must allow topic/$aws/things/%s/shadow/*",
             thing);
    return ESP_OK;
}

void shadow_mark_dirty(void)
{
    s_dirty = true;
}

void shadow_publish_now(void)
{
    if (!s_pay_lock) {
        return;                          /* 还没启动 */
    }
    publish_reported(false, 0);
    s_dirty = false;
}

const char *shadow_last_cmd_result(void)
{
    return s_cmd_result;
}

int shadow_report_interval_s(void)
{
    return s_interval_s;
}

void shadow_inject_delta(const char *json)
{
    if (!json || json[0] == '\0') {
        ESP_LOGW(TAG, "inject: empty JSON");
        return;
    }
    if (!s_pay_lock) {
        ESP_LOGW(TAG, "inject: shadow not started yet");
        return;
    }
    ESP_LOGW(TAG, "inject delta (local, bypassing MQTT): %s", json);
    on_delta(json, (int)strlen(json));
}
