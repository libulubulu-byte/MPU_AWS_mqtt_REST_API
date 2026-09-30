/* time_sync.c - 时间服务实现（详见 time_sync.h）
 *
 * 三条时间线要分清，否则调试时会绕晕：
 *   1. 系统时间  : settimeofday() 设置，time() 读取，被 RTC 计时器推进；
 *   2. NVS 备份  : epoch 落盘，只在**上电**时用来把时间"找回来"；
 *   3. SNTP      : 联网后校准系统时间，是唯一的"事实来源"。
 *
 * 并发约定：所有 NVS 写入都发生在同一个任务里（本文件起的 1Hz 任务）。
 * SNTP 的 sync 回调运行在 lwIP 的线程上下文里，那里**只置标志**、不碰 NVS ——
 * 在协议栈线程里做 flash 写会阻塞收包，是实打实的坑。
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>       /* settimeofday / struct timeval */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "time_sync.h"
#include "app_config.h"

static const char *TAG = "time";

#define NVS_NS      "rtc"
#define NVS_K_EPOCH "epoch"

/* 服务器列表的**元素个数必须等于 CONFIG_LWIP_SNTP_MAX_SERVERS**：
 * esp_sntp_config_t.servers 是编译期定长数组，给多了是编译错误，
 * 给少了只是浪费（但服务器冗余就没了）。这里固定 3 个，sdkconfig 也是 3。 */
#define SNTP_SERVER_COUNT 3

static time_src_t s_src = TIME_SRC_NONE;
static volatile bool s_synced = false;       /* 被 SNTP 校准过 */
static volatile bool s_pending_save = false; /* sync 回调要求立刻落盘 */
static volatile bool s_sntp_started = false;
static volatile uint32_t s_sync_count = 0;   /* 成功校时次数，漂移观测用 */
static int64_t s_last_saved = 0;             /* 上次落盘的 epoch */

/* ------------------------------------------------------------------ */
/* NVS 读写                                                            */
/* ------------------------------------------------------------------ */

static void nvs_read_epoch(int64_t *out)
{
    *out = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_i64(h, NVS_K_EPOCH, out) != ESP_OK) {
        *out = 0;
    }
    nvs_close(h);
}

/* 只在 1Hz 任务（或显式调用）里被调用，不做额外加锁。 */
static esp_err_t nvs_write_epoch(time_t epoch)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i64(h, NVS_K_EPOCH, (int64_t)epoch);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_last_saved = (int64_t)epoch;
    }
    return err;
}

/* 按需落盘：force 为真则无条件写；否则只有相对上次落盘的变化量达到
 * APP_TIME_SAVE_S 才写（省 flash 擦写寿命）。 */
static void save_if_due(bool force)
{
    time_t now = time(NULL);
    if ((int64_t)now < (int64_t)APP_TIME_MIN_VALID) {
        return;                       /* 1970 不值得写，写进去反而有害 */
    }
    if (!force && llabs((long long)now - (long long)s_last_saved) < APP_TIME_SAVE_S) {
        return;
    }
    if (nvs_write_epoch(now) == ESP_OK) {
        ESP_LOGD(TAG, "epoch %lld saved (src=%s)", (long long)now, time_sync_src_str());
    }
}

/* ------------------------------------------------------------------ */
/* 对外查询                                                            */
/* ------------------------------------------------------------------ */

time_t time_sync_now(void)
{
    return time(NULL);
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

bool time_sync_is_valid(void)
{
    return (int64_t)time(NULL) >= (int64_t)APP_TIME_MIN_VALID;
}

time_src_t time_sync_src(void)
{
    return s_src;
}

const char *time_sync_src_str(void)
{
    switch (s_src) {
    case TIME_SRC_SNTP: return "sntp";
    case TIME_SRC_NVS:  return "nvs";
    default:            return "none";
    }
}

const char *time_sync_iso(time_t epoch, char *buf, size_t sz)
{
    struct tm tmv;
    if (localtime_r(&epoch, &tmv) == NULL) {
        snprintf(buf, sz, "invalid");
        return buf;
    }
    if (strftime(buf, sz, "%Y-%m-%dT%H:%M:%S", &tmv) == 0) {
        snprintf(buf, sz, "invalid");
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* 设置时间                                                            */
/* ------------------------------------------------------------------ */

esp_err_t time_sync_set_epoch(time_t epoch, time_src_t src)
{
    if ((int64_t)epoch < (int64_t)APP_TIME_MIN_VALID) {
        ESP_LOGW(TAG, "refusing to set implausible epoch %lld", (long long)epoch);
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    s_src = src;
    s_synced = (src == TIME_SRC_SNTP);
    char iso[32];
    ESP_LOGI(TAG, "time set: %lld (%s, src=%s)", (long long)epoch,
             time_sync_iso(epoch, iso, sizeof(iso)), time_sync_src_str());
    return ESP_OK;
}

esp_err_t time_sync_save_now(void)
{
    return nvs_write_epoch(time(NULL));
}

/* ------------------------------------------------------------------ */
/* SNTP                                                                */
/* ------------------------------------------------------------------ */

/* 运行在 lwIP 线程：只置标志，不写 NVS、不做耗时操作。 */
static void on_sntp_sync(struct timeval *tv)
{
    s_synced = true;
    s_src = TIME_SRC_SNTP;
    s_sync_count++;
    s_pending_save = true;          /* 由 1Hz 任务真正落盘 */

    char iso[32];
    ESP_LOGI(TAG, "SNTP synced (#%u): epoch=%lld (%s)",
             (unsigned)s_sync_count, (long long)tv->tv_sec,
             time_sync_iso((time_t)tv->tv_sec, iso, sizeof(iso)));
}

esp_err_t time_sync_start_sntp(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }

    esp_sntp_config_t cfg =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(SNTP_SERVER_COUNT,
            ESP_SNTP_SERVER_LIST(APP_SNTP_SERVER_1,
                                 APP_SNTP_SERVER_2,
                                 APP_SNTP_SERVER_3));
    cfg.sync_cb = on_sntp_sync;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started: %s, %s, %s (resync every %d s)",
             APP_SNTP_SERVER_1, APP_SNTP_SERVER_2, APP_SNTP_SERVER_3,
             CONFIG_LWIP_SNTP_UPDATE_DELAY / 1000);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 初始化与后台任务                                                    */
/* ------------------------------------------------------------------ */

esp_err_t time_sync_init(void)
{
    /* 时区：闹钟按本地时间语义，报文体里的 epoch 一律 UTC。
     * POSIX TZ 的符号与直觉相反 —— 东八区写 "CST-8"。 */
    setenv("TZ", APP_TZ, 1);
    tzset();

    int64_t saved = 0;
    nvs_read_epoch(&saved);

    if (saved >= (int64_t)APP_TIME_MIN_VALID) {
        struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_src = TIME_SRC_NVS;
        s_synced = false;               /* 还没被校准过，别谎报 */
        s_last_saved = saved;           /* 启动时值等于落盘值，不需要立刻重写 */

        char iso[32];
        ESP_LOGW(TAG, "time restored from NVS: %lld (%s) - running on the RTC, "
                      "NOT yet verified (offline interval is unknown)",
                 (long long)saved, time_sync_iso((time_t)saved, iso, sizeof(iso)));
    } else {
        s_src = TIME_SRC_NONE;
        s_synced = false;
        ESP_LOGW(TAG, "no usable time in NVS - clock reads 1970 until SNTP syncs "
                      "(the alarm engine stays idle: %ld is the minimum)",
                 (long)APP_TIME_MIN_VALID);
    }
    return ESP_OK;
}

static void time_task(void *arg)
{
    (void)arg;
    int save_div = 0;
    int log_div = 0;

    for (;;) {
        /* 1. SNTP 刚校准过 -> 立刻落盘（sync 回调只置了标志） */
        if (s_pending_save) {
            s_pending_save = false;
            save_if_due(true);
        }

        /* 2. 每 10s 检查一次是否到了该落盘的间隔 */
        if (++save_div >= 10) {
            save_div = 0;
            save_if_due(false);
        }

        /* 3. 周期打印 "TIME ..." —— 离线漂移测量的原始数据。
         *    抓串口脚本会给每行加 PC 墙钟前缀，两者一减就是实时误差。
         *
         *    **带毫秒**。只到秒的话量化误差就有 ±1s，而片内 RC 短期漂移
         *    可能不到 1s，测出来的全是噪声。加上毫秒后量化误差降到
         *    ±1ms，剩下的固定偏移（约 1s）来自串口传输与行缓冲延迟 ——
         *    它是**恒定**的，所以看"偏移随时间的**变化量**"即可，
         *    绝对值不参与判断。 */
        if (++log_div >= APP_TIME_LOG_S) {
            log_div = 0;
            struct timeval tv;
            gettimeofday(&tv, NULL);
            char iso[40];
            time_sync_iso(tv.tv_sec, iso, sizeof(iso));
            /* 毫秒必须拼在毫秒后面 —— time_sync_iso() 输出的是整秒
             * "2026-09-14T13:51:00"，直接接 ".%03d" 会得到一串长数字，
             * 解析脚本按 ISO 取秒时会被它带偏。 */
            ESP_LOGI(TAG, "TIME epoch=%lld iso=%s.%03d src=%s synced=%d up=%lld",
                     (long long)tv.tv_sec, iso, (int)(tv.tv_usec / 1000),
                     time_sync_src_str(), (int)s_synced,
                     (long long)(esp_timer_get_time() / 1000000));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t time_sync_start_tasks(void)
{
    if (xTaskCreate(time_task, "time_sync", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
