/* alarm.c - 离线闹钟引擎实现（详见 alarm.h）
 *
 * 引擎是一个 1Hz 的任务，每拍做三件事：
 *   1. 日期变了就清空"今天已响过"的位图；
 *   2. 找出"时:分匹配 + 今天该响 + 今天没响过"的闹钟 -> 触发；
 *   3. 在响的闹钟超时就停。
 *
 * 重复触发的两道保险（都必要，缺一个测试时就会看到重复响）：
 *   - 内存位图 s_fired_mask：挡住同一分钟内 60 次判定命中的后 59 次；
 *   - 持久化的 s_last_fire_epoch + id：挡住"响铃瞬间掉电重启、又落在
 *     同一分钟内"的重复。位图重启就没了，只有落盘的时间戳能兜住。
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "alarm.h"
#include "time_sync.h"
#include "lamp.h"

static const char *TAG = "alarm";

#define NVS_NS            "alarm"
#define NVS_K_BLOB        "blob"
#define NVS_K_LF_EPOCH    "lf_epoch"
#define NVS_K_LF_ID       "lf_id"

#define ALARM_BLOB_MAGIC  0x414C524DUL   /* "ALRM" */
#define ALARM_BLOB_VERSION 1

/* 同一闹钟在这么多秒内不重复触发（兜住"响铃瞬间重启"的场景） */
#define ALARM_REFIRE_GUARD_S 90

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    alarm_t  list[ALARM_SLOTS];
} alarm_blob_t;

static alarm_t  s_alarms[ALARM_SLOTS];
static alarm_state_t s_state = ALARM_ST_IDLE;
static time_t   s_ring_until = 0;
static time_t   s_last_fire = 0;
static uint8_t  s_last_fire_id = 0xFF;
static uint32_t s_fired_ymd = 0;        /* 内存位图对应的日期 */
static uint8_t  s_fired_mask = 0;       /* bit idx = 该槽今天已响过 */
static bool     s_missed_checked = false;
static void   (*s_change_cb)(void) = NULL;
static TaskHandle_t s_task = NULL;

static void notify_change(void)
{
    if (s_change_cb) {
        s_change_cb();
    }
}

/* ------------------------------------------------------------------ */
/* 落盘                                                                */
/* ------------------------------------------------------------------ */

static void blob_save(void)
{
    alarm_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = ALARM_BLOB_MAGIC;
    b.version = ALARM_BLOB_VERSION;
    b.count = ALARM_SLOTS;
    memcpy(b.list, s_alarms, sizeof(s_alarms));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed - alarm config not persisted");
        return;
    }
    esp_err_t err = nvs_set_blob(h, NVS_K_BLOB, &b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
}

static void blob_load(void)
{
    memset(s_alarms, 0, sizeof(s_alarms));
    for (int i = 0; i < ALARM_SLOTS; i++) {
        s_alarms[i].id = (uint8_t)i + 1;
        s_alarms[i].duration_s = APP_ALARM_DURATION_S;
        s_alarms[i].days = ALARM_DAY_ALL;
    }

    alarm_blob_t b;
    nvs_handle_t h;
    size_t len = sizeof(b);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no alarm config in NVS - starting with %d empty slots",
                 ALARM_SLOTS);
        return;
    }
    esp_err_t err = nvs_get_blob(h, NVS_K_BLOB, &b, &len);
    nvs_close(h);

    /* NVS 自己带 CRC，这里只需要挡住"格式变了"（magic/version 不匹配）。
     * 不匹配就当没配过，比拿旧布局硬解出一堆乱码闹钟安全得多。 */
    if (err != ESP_OK || len != sizeof(b) ||
        b.magic != ALARM_BLOB_MAGIC || b.version != ALARM_BLOB_VERSION) {
        ESP_LOGW(TAG, "alarm blob missing/incompatible (%s, %u bytes) - "
                      "starting with empty slots", esp_err_to_name(err),
                 (unsigned)len);
        return;
    }
    memcpy(s_alarms, b.list, sizeof(s_alarms));
    ESP_LOGI(TAG, "alarm config loaded from NVS");
}

static void last_fire_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int64_t e = 0;
    uint8_t id = 0xFF;
    if (nvs_get_i64(h, NVS_K_LF_EPOCH, &e) == ESP_OK) {
        s_last_fire = (time_t)e;
    }
    if (nvs_get_u8(h, NVS_K_LF_ID, &id) == ESP_OK) {
        s_last_fire_id = id;
    }
    nvs_close(h);
}

static void last_fire_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i64(h, NVS_K_LF_EPOCH, (int64_t)s_last_fire);
    nvs_set_u8(h, NVS_K_LF_ID, s_last_fire_id);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* 查询                                                                */
/* ------------------------------------------------------------------ */

const alarm_t *alarm_get(int idx)
{
    if (idx < 0 || idx >= ALARM_SLOTS) {
        return NULL;
    }
    return &s_alarms[idx];
}

int alarm_find_free(void)
{
    for (int i = 0; i < ALARM_SLOTS; i++) {
        if (!s_alarms[i].enable) {
            return i;
        }
    }
    return -1;
}

alarm_state_t alarm_state(void)
{
    return s_state;
}

const char *alarm_state_str(void)
{
    switch (s_state) {
    case ALARM_ST_RINGING: return "ringing";
    case ALARM_ST_MISSED:  return "missed";
    default:               return "idle";
    }
}

int alarm_ringing_remaining_s(void)
{
    if (s_state != ALARM_ST_RINGING) {
        return 0;
    }
    time_t now = time(NULL);
    if (s_ring_until <= now) {
        return 0;
    }
    return (int)(s_ring_until - now);
}

time_t alarm_last_fire(void)
{
    return s_last_fire;
}

/* 找出 (hour, minute) 在 now 之后最近的一次出现时刻。
 * days 为 0 表示不限制，用于"下一次触发"的展示。 */
static time_t next_occurrence(int hour, int minute, uint8_t days, time_t now)
{
    struct tm base;
    localtime_r(&now, &base);

    for (int d = 0; d < 8; d++) {
        struct tm c = base;
        c.tm_mday += d;
        c.tm_hour = hour;
        c.tm_min = minute;
        c.tm_sec = 0;
        c.tm_isdst = -1;              /* 让 mktime 自己判断夏令时 */

        time_t cand = mktime(&c);     /* mktime 会规范化 tm_mday 溢出 */
        if (cand == (time_t)-1) {
            continue;
        }
        /* 归一化后再取一次星期，跨月/跨年时 base 的 tm_wday 已经不准了 */
        struct tm norm;
        localtime_r(&cand, &norm);
        if (days && !(days & (1u << norm.tm_wday))) {
            continue;
        }
        if (cand <= now) {
            continue;                 /* 今天这个点已经过了 */
        }
        return cand;
    }
    return 0;
}

time_t alarm_next_fire(void)
{
    if (!time_sync_is_valid()) {
        return 0;
    }
    time_t now = time(NULL);
    time_t best = 0;

    for (int i = 0; i < ALARM_SLOTS; i++) {
        const alarm_t *a = &s_alarms[i];
        if (!a->enable) {
            continue;
        }
        time_t cand = next_occurrence(a->hour, a->minute, a->days, now);
        if (cand && (best == 0 || cand < best)) {
            best = cand;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* 配置写入                                                            */
/* ------------------------------------------------------------------ */

static bool alarm_valid(const alarm_t *a)
{
    return a->hour < 24 && a->minute < 60 &&
           (a->days & ALARM_DAY_ALL) != 0 &&
           a->duration_s >= 1 && a->duration_s <= 3600;
}

esp_err_t alarm_set(int idx, const alarm_t *a)
{
    if (idx < 0 || idx >= ALARM_SLOTS || a == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (a->enable && !alarm_valid(a)) {
        ESP_LOGW(TAG, "rejected invalid alarm: %02u:%02u days=0x%02X dur=%u",
                 (unsigned)a->hour, (unsigned)a->minute, (unsigned)a->days,
                 (unsigned)a->duration_s);
        return ESP_ERR_INVALID_ARG;
    }

    s_alarms[idx] = *a;
    if (s_alarms[idx].id == 0) {
        s_alarms[idx].id = (uint8_t)idx + 1;
    }
    blob_save();

    /* 配了新闹钟后，"错过"这个结论就不成立了，回到 idle */
    if (s_state == ALARM_ST_MISSED) {
        s_state = ALARM_ST_IDLE;
    }
    ESP_LOGI(TAG, "alarm[%d] set: %s %02u:%02u days=0x%02X dur=%us -> next=%lld",
             idx, s_alarms[idx].enable ? "on " : "off",
             (unsigned)s_alarms[idx].hour, (unsigned)s_alarms[idx].minute,
             (unsigned)s_alarms[idx].days, (unsigned)s_alarms[idx].duration_s,
             (long long)alarm_next_fire());
    notify_change();
    return ESP_OK;
}

esp_err_t alarm_clear(int idx)
{
    if (idx < 0 || idx >= ALARM_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    alarm_t a = { .id = s_alarms[idx].id, .enable = false,
                  .duration_s = APP_ALARM_DURATION_S, .days = ALARM_DAY_ALL };
    return alarm_set(idx, &a);
}

void alarm_set_change_cb(void (*cb)(void))
{
    s_change_cb = cb;
}

esp_err_t alarm_save(void)
{
    blob_save();
    return ESP_OK;
}

esp_err_t alarm_test_fire(int seconds)
{
    if (seconds < 1 || seconds > 600) {
        return ESP_ERR_INVALID_ARG;
    }
    s_state = ALARM_ST_RINGING;
    s_ring_until = time(NULL) + seconds;
    lamp_indicator_start(255, 0, 0, 200, 200, (uint32_t)seconds * 1000);
    ESP_LOGW(TAG, "*** ALARM TEST FIRE *** %d s (nothing persisted, "
                  "no effect on real alarms)", seconds);
    notify_change();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 触发                                                                */
/* ------------------------------------------------------------------ */

static void alarm_fire(int idx, const alarm_t *a, time_t now)
{
    s_state = ALARM_ST_RINGING;
    s_ring_until = now + a->duration_s;
    s_last_fire = now;
    s_last_fire_id = a->id;
    s_fired_mask |= (uint8_t)(1u << idx);
    last_fire_save();

    /* 触铃：板载 WS2812 红闪。lamp 的指示器优先级高于 lamp_set()，
     * 所以即使这期间云端下发开关灯，闪烁也不会被打断。 */
    lamp_indicator_start(255, 0, 0, 200, 200, (uint32_t)a->duration_s * 1000);

    char iso[32];
    ESP_LOGW(TAG, "*** ALARM FIRE *** id=%u %02u:%02u for %us at %s "
                  "(network NOT involved)",
             (unsigned)a->id, (unsigned)a->hour, (unsigned)a->minute,
             (unsigned)a->duration_s,
             time_sync_iso(now, iso, sizeof(iso)));
    notify_change();
}

static void alarm_stop_ringing(void)
{
    s_state = ALARM_ST_IDLE;
    s_ring_until = 0;
    lamp_indicator_stop();
    ESP_LOGI(TAG, "alarm ringing done");
    notify_change();
}

/* 时间首次可比之后跑一次：今天已经过去超过容忍窗的闹钟标记为 missed。
 * 只跑一次 —— 设备运行期间不存在"错过"，错过只发生在上电/恢复时。 */
static void check_missed_once(void)
{
    if (s_missed_checked || !time_sync_is_valid()) {
        return;
    }
    s_missed_checked = true;

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    for (int i = 0; i < ALARM_SLOTS; i++) {
        const alarm_t *a = &s_alarms[i];
        if (!a->enable || !(a->days & (1u << t.tm_wday))) {
            continue;
        }
        struct tm at = t;
        at.tm_hour = a->hour;
        at.tm_min = a->minute;
        at.tm_sec = 0;
        at.tm_isdst = -1;
        time_t at_epoch = mktime(&at);
        if (at_epoch == (time_t)-1) {
            continue;
        }

        long late = (long)(now - at_epoch);
        if (late > 0 && late <= APP_ALARM_GRACE_S) {
            /* 刚过一点点（设备刚恢复），补响 —— 这正是容忍窗口的用途 */
            ESP_LOGW(TAG, "alarm[%d] is %lds late but inside the %ds grace "
                          "window - firing now",
                     i, late, APP_ALARM_GRACE_S);
            alarm_fire(i, a, now);
            return;
        }
        if (late > APP_ALARM_GRACE_S) {
            /* 早就过了：标记 missed，**不补响**。半夜上电把当天所有闹钟
             * 连环放一遍，比漏响更糟糕。 */
            s_state = ALARM_ST_MISSED;
            char iso[32];
            ESP_LOGW(TAG, "alarm[%d] missed today (%02u:%02u was %lds ago) - "
                          "marking missed, will NOT fire late",
                     i, a->hour, a->minute, late);
            ESP_LOGW(TAG, "  (now=%s)", time_sync_iso(now, iso, sizeof(iso)));
            notify_change();
            return;
        }
    }
}

static void alarm_task(void *arg)
{
    (void)arg;
    for (;;) {
        check_missed_once();

        if (time_sync_is_valid()) {
            time_t now = time(NULL);
            struct tm t;
            localtime_r(&now, &t);

            uint32_t ymd = (uint32_t)(t.tm_year + 1900) * 10000u
                         + (uint32_t)(t.tm_mon + 1) * 100u
                         + (uint32_t)t.tm_mday;
            if (ymd != s_fired_ymd) {
                s_fired_ymd = ymd;
                s_fired_mask = 0;
                if (s_state == ALARM_ST_MISSED) {
                    s_state = ALARM_ST_IDLE;   /* 新的一天，错过状态翻篇 */
                }
            }

            for (int i = 0; i < ALARM_SLOTS; i++) {
                const alarm_t *a = &s_alarms[i];
                if (!a->enable) {
                    continue;
                }
                if (!(a->days & (1u << t.tm_wday))) {
                    continue;
                }
                if (a->hour != t.tm_hour || a->minute != t.tm_min) {
                    continue;
                }
                if (s_fired_mask & (1u << i)) {
                    continue;                 /* 今天这一分钟内已经响过 */
                }
                /* 兜底：同一条闹钟 90s 内不重复（响铃瞬间掉电重启的情况） */
                if (s_last_fire_id == a->id && s_last_fire > 0 &&
                    (now - s_last_fire) < ALARM_REFIRE_GUARD_S) {
                    ESP_LOGW(TAG, "alarm[%d] suppressed: fired %lds ago",
                             i, (long)(now - s_last_fire));
                    s_fired_mask |= (uint8_t)(1u << i);
                    continue;
                }
                alarm_fire(i, a, now);
            }

            /* 响铃到点收尾。指示器本身也会到期，但这里显式停一次，
             * 让灯立刻回到逻辑状态（而不是等闪烁周期跑完）。 */
            if (s_state == ALARM_ST_RINGING && now >= s_ring_until) {
                alarm_stop_ringing();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t alarm_init(void)
{
    blob_load();
    last_fire_load();

    for (int i = 0; i < ALARM_SLOTS; i++) {
        const alarm_t *a = &s_alarms[i];
        if (a->enable) {
            ESP_LOGI(TAG, "  slot %d: ON  %02u:%02u days=0x%02X dur=%us id=%u",
                     i, (unsigned)a->hour, (unsigned)a->minute,
                     (unsigned)a->days, (unsigned)a->duration_s,
                     (unsigned)a->id);
        } else {
            ESP_LOGI(TAG, "  slot %d: off", i);
        }
    }

    if (xTaskCreate(alarm_task, "alarm", 4096, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "alarm engine running (1 Hz, local time only, TZ=%s, "
                  "min_valid_epoch=%ld)", APP_TZ, (long)APP_TIME_MIN_VALID);
    return ESP_OK;
}
