/* alarm.h - 离线闹钟引擎（完全本地，不依赖网络）
 *
 * 设计要点：
 *   1. **与网络无关**。引擎只看本地时间，WiFi 断了、MQTT 连不上、云平台
 *      挂了，闹钟照响。这是"离线闹钟"这个需求的全部意义所在。
 *   2. 落盘在 NVS，断电重启后闹钟配置还在，且当天已响过的不会重复响。
 *   3. 时间不可信（1970）时引擎空转不响 —— 与其在错误的时间响，不如不响。
 *
 * 闹钟按**本地时间**语义（时区见 APP_TZ）："每天 07:30" 指本地 07:30。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_SLOTS      APP_ALARM_MAX
#define ALARM_DAY_SUN    0x01
#define ALARM_DAY_MON    0x02
#define ALARM_DAY_TUE    0x04
#define ALARM_DAY_WED    0x08
#define ALARM_DAY_THU    0x10
#define ALARM_DAY_FRI    0x20
#define ALARM_DAY_SAT    0x40
#define ALARM_DAY_ALL    0x7F

/* ⚠️ 这个结构体会被**原样**写进 NVS blob。改字段等于改落盘格式，
 * 必须同时递增 alarm.c 里的 ALARM_BLOB_VERSION，否则老数据会被误读。 */
typedef struct {
    uint8_t  id;          /* 用户可见编号，desired 里靠它指定改哪一条 */
    bool     enable;
    uint8_t  hour;        /* 0-23，本地时间 */
    uint8_t  minute;      /* 0-59 */
    uint8_t  days;        /* bit0=周日(与 struct tm 的 tm_wday 对齐) ... bit6=周六 */
    uint16_t duration_s;  /* 响铃时长（秒） */
} alarm_t;

typedef enum {
    ALARM_ST_IDLE = 0,
    ALARM_ST_RINGING,
    ALARM_ST_MISSED,      /* 到点时设备没在跑（断电/重启），已错过 */
} alarm_state_t;

/* 初始化：从 NVS 载入配置，起 1Hz 引擎任务。**必须在 WiFi 之前调用** ——
 * 离线场景下它是唯一能保证闹钟可用的初始化路径。 */
esp_err_t alarm_init(void);

/* 取第 idx 个槽（0..ALARM_SLOTS-1），越界返回 NULL。 */
const alarm_t *alarm_get(int idx);

/* 写入第 idx 个槽并落盘。字段非法（hour>23 等）返回 ESP_ERR_INVALID_ARG。
 * 触发变化回调（用于让 Shadow 标脏并回读上报）。 */
esp_err_t alarm_set(int idx, const alarm_t *a);

/* 停用并清空第 idx 个槽。 */
esp_err_t alarm_clear(int idx);

/* 找第一个未启用的槽，-1 表示全满。 */
int alarm_find_free(void);

/* 最近一次将要触发的时刻（epoch），没有则返回 0。 */
time_t alarm_next_fire(void);

/* 最近一次实际触发的时刻（epoch），从没响过返回 0。 */
time_t alarm_last_fire(void);

alarm_state_t alarm_state(void);
const char *alarm_state_str(void);

/* 响铃剩余秒数，未在响铃时返回 0。 */
int alarm_ringing_remaining_s(void);

/* 注册"状态或配置发生变化"回调（响铃开始/结束、配置被改）。
 * 用于让 shadow 模块置脏标志 —— 由 alarm 主动通知，避免 shadow 轮询。 */
void alarm_set_change_cb(void (*cb)(void));

/* 立刻响铃 seconds 秒，用于现场验证触铃通路（灯 + 影子上报）。
 * **不写 NVS、不参与去重**，所以不会影响任何真实闹钟的后续触发。 */
esp_err_t alarm_test_fire(int seconds);

/* 直接写 NVS 落盘（串口改完配置后手动调一次）。 */
esp_err_t alarm_save(void);

#ifdef __cplusplus
}
#endif
