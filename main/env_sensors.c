/* env_sensors.c - 环境传感器应用层（实现）
 *
 * 业务规则全部集中在本文件。驱动层（mpu6050/dht22/hcsr04/pir）不感知它们。
 * 详见 env_sensors.h 的分层说明。
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "env_sensors.h"
#include "sht30.h"
#include "mpu6050.h"
#include "dht22.h"
#include "hcsr04.h"
#include "pir.h"
#include "lamp.h"
#include "rest_api.h"

static const char *TAG = "env";

/* ------------------------------------------------------------------ */
/* 业务参数：全部来自 Kconfig，改 menuconfig 即可，不用动代码           */
/* ------------------------------------------------------------------ */

#define TICK_MS              CONFIG_APP_ENV_SAMPLE_MS

/* 各传感器每隔几个 tick 采一次 */
#define MPU_EVERY_TICKS      1
#define PIR_EVERY_TICKS      2
#define DIST_EVERY_TICKS     10
#define DHT_EVERY_TICKS      40
#define SHT30_EVERY_TICKS    40

/* 统一状态行的周期（ms）。0 表示关闭周期打印。
 * 由毫秒换算成 tick，四舍五入并且至少 1 —— 否则 0 会被当成"每 tick 都打"。 */
#define PRINT_MS             CONFIG_APP_ENV_PRINT_MS
#define PRINT_EVERY_TICKS    ((PRINT_MS) > 0 ? \
                              (((PRINT_MS) + (TICK_MS) / 2) / (TICK_MS)) : 0)

/* 震动判定 */
#define VIB_THRESH_MG        CONFIG_APP_MPU6050_VIB_THRESH_MG
#define VIB_CONSEC           CONFIG_APP_MPU6050_VIB_CONSEC
#define VIB_COOLDOWN_MS      CONFIG_APP_MPU6050_VIB_COOLDOWN_MS

/* PIR 保持时间（存在感去抖） */
#define PIR_HOLD_MS          CONFIG_APP_PIR_HOLD_MS

/* PIR 绿闪的节奏与总时长。
 *
 * PIR 是"raw 高电平时一直闪、变低停"，所以结束由 lamp_indicator_stop()
 * 决定，而不是靠定时到期。这里的 total_ms 只作一个兜底上限，取一个长到
 * 不可能提前到期的值（24h）—— 若它中途到期，s_ind_active 被清掉后
 * 下一 tick 会重开，相位归零，闪烁节奏会卡一下。真正的停止靠 stop()。 */
#define PIR_BLINK_ON_MS      150
#define PIR_BLINK_OFF_MS     150
#define PIR_BLINK_FOREVER_MS (24u * 60u * 60u * 1000u)

/* 告警阈值 */
#define ALERT_TEMP_MAX       CONFIG_APP_ALERT_TEMP_MAX
#define ALERT_TEMP_MIN       CONFIG_APP_ALERT_TEMP_MIN
#define ALERT_HUM_MAX        CONFIG_APP_ALERT_HUM_MAX
#define ALERT_HUM_MIN        CONFIG_APP_ALERT_HUM_MIN
#define ALERT_DIST_MIN       CONFIG_APP_ALERT_DIST_MIN

/* 滞回量：告警置位后要回落这么多才复位。
 * 没有它，读数卡在阈值上会让灯一直闪、上报一直翻转。 */
#define HYST_TEMP_C          2.0f
#define HYST_HUM_PCT         3.0f
#define HYST_DIST_CM         5.0f

/* 告警时灯的闪烁参数 */
#define ALERT_BLINK_ON_MS    150
#define ALERT_BLINK_OFF_MS   150
#define ALERT_BLINK_TOTAL_MS 3000

/* ------------------------------------------------------------------ */
/* 内部状态                                                            */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_lock = NULL;     /* 保护 s_snap */
static env_snapshot_t    s_snap = {
    .alerts = 0,
};

static TaskHandle_t s_task = NULL;
static bool s_started = false;

/* --- MPU6050 震动检测状态 --- */
static uint32_t s_vib_consec = 0;           /* 连续超阈次数 */
static int64_t  s_vib_last_report_us = 0;   /* 上次上报时刻，用于冷却 */

/* --- PIR 状态 --- */
static bool    s_pir_raw_prev = false;      /* 上一次读到的原始电平 */
static bool    s_presence = false;          /* 保持窗口内的存在状态 */
static int64_t s_pir_last_seen_us = 0;      /* 最近一次检出时刻 */
static bool    s_pir_blinking = false;      /* PIR 是否正开着指示灯绿闪 */

/* --- 告警状态（用于判断"翻转"） --- */
static uint32_t s_alerts_prev = 0;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* 取快照的锁；未初始化时返回 false */
static bool lock(void)
{
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* 关闭告警时恢复普通灯色（绿/灭），只在告警从"有"变"无"时调用 */
static void lamp_restore(void)
{
    lamp_indicator_stop();
}

const char *env_alert_name(uint32_t alerts)
{
    if (alerts == 0)                     return "none";
    if (alerts & ENV_ALERT_TEMP_HIGH)    return "temp_high";
    if (alerts & ENV_ALERT_TEMP_LOW)     return "temp_low";
    if (alerts & ENV_ALERT_HUM_HIGH)     return "hum_high";
    if (alerts & ENV_ALERT_HUM_LOW)      return "hum_low";
    if (alerts & ENV_ALERT_DIST_NEAR)    return "dist_near";
    return "other";
}

/* ------------------------------------------------------------------ */
/* 告警状态机                                                          */
/* ------------------------------------------------------------------ */

/* 带滞回的阈值判断：置位用阈值，复位用阈值 ∓ 滞回量。
 * cur_alert 是上一次该位的状态，用于决定用哪个边界。 */
static bool apply_hyst(bool is_high_side, float value, float thresh,
                       float hyst, bool cur_alert)
{
    if (is_high_side) {
        /* 高告警：> 阈值置位，< 阈值-滞回 才复位 */
        return cur_alert ? (value > thresh - hyst) : (value > thresh);
    }
    /* 低告警：< 阈值置位，> 阈值+滞回 才复位 */
    return cur_alert ? (value < thresh + hyst) : (value < thresh);
}

/* 根据快照重算告警位。调用者必须已持锁。
 * 返回 true 表示告警位发生了变化（用于决定要不要闪灯/补报）。 */
static bool recompute_alerts_locked(void)
{
    uint32_t a = 0;

    /* 温湿度：优先用 DHT22，它读不到时退回 SHT30，
     * 这样断一个传感器不至于完全没有温湿度告警能力。 */
    float t = 0.0f, h = 0.0f;
    bool have_t = false, have_h = false;
    if (s_snap.dht_ok) {
        t = s_snap.dht_temp_c;
        h = s_snap.dht_hum_pct;
        have_t = have_h = true;
    }

    if (have_t) {
        if (apply_hyst(true, t, ALERT_TEMP_MAX, HYST_TEMP_C,
                       (s_alerts_prev & ENV_ALERT_TEMP_HIGH) != 0)) {
            a |= ENV_ALERT_TEMP_HIGH;
        }
        if (apply_hyst(false, t, ALERT_TEMP_MIN, HYST_TEMP_C,
                       (s_alerts_prev & ENV_ALERT_TEMP_LOW) != 0)) {
            a |= ENV_ALERT_TEMP_LOW;
        }
    }
    if (have_h) {
        if (apply_hyst(true, h, ALERT_HUM_MAX, HYST_HUM_PCT,
                       (s_alerts_prev & ENV_ALERT_HUM_HIGH) != 0)) {
            a |= ENV_ALERT_HUM_HIGH;
        }
        if (apply_hyst(false, h, ALERT_HUM_MIN, HYST_HUM_PCT,
                       (s_alerts_prev & ENV_ALERT_HUM_LOW) != 0)) {
            a |= ENV_ALERT_HUM_LOW;
        }
    }

    /* 距离过近（0 = 关闭该告警） */
    if (ALERT_DIST_MIN > 0 && s_snap.dist_ok) {
        if (apply_hyst(false, s_snap.distance_cm, (float)ALERT_DIST_MIN,
                       HYST_DIST_CM,
                       (s_alerts_prev & ENV_ALERT_DIST_NEAR) != 0)) {
            a |= ENV_ALERT_DIST_NEAR;
        }
    }

    bool changed = (a != s_alerts_prev);
    s_snap.alerts = a;
    /* ⚠️ 必须同步 s_alerts_prev，否则 changed 恒为 true：
     * 下一轮拿 a 跟这份没被更新的旧值比，永远"不一样"，
     * 于是每 tick 都重打 ALERT 日志 + 重启闪灯（会把串口刷满），
     * 而且 apply_hyst() 的 cur_alert 也永远读到 0，滞回等于没接。 */
    s_alerts_prev = a;
    return changed;
}

/* ------------------------------------------------------------------ */
/* 采集：各传感器                                                        */
/* ------------------------------------------------------------------ */

/* MPU6050：读一次并按"连续 N 次超阈 + 冷却"判定震动。
 * 返回 true 表示这一轮确认了一次震动。 */
static bool sample_mpu(void)
{
    mpu6050_data_t d;
    if (!mpu6050_read(&d)) {
        /* 读失败时把 imu_ok 置 false，状态行如实显示 "--" */
        if (lock()) {
            s_snap.imu_ok = false;
            unlock();
        }
        return false;
    }

    float dev = mpu6050_accel_deviation_mg(&d);
    bool over = (dev >= (float)VIB_THRESH_MG);

    if (lock()) {
        s_snap.imu_ok = true;
        /* 六轴 + 温度全存下来：状态行要完整显示，不能只留派生量。
         * dev 是震动判据，和原始读数一起放进同一份快照，保证状态行里
         * 的 ax..gz 和它来自同一次采样（否则会看到"数值在动但偏差不变"）。 */
        s_snap.imu_ax = d.ax;
        s_snap.imu_ay = d.ay;
        s_snap.imu_az = d.az;
        s_snap.imu_gx = d.gx;
        s_snap.imu_gy = d.gy;
        s_snap.imu_gz = d.gz;
        s_snap.imu_temp_c = d.temp_c;
        s_snap.accel_dev_mg = dev;
        unlock();
    }

    if (!over) {
        s_vib_consec = 0;               /* 未超阈，抖动计数清零 */
        return false;
    }

    s_vib_consec++;
    if (s_vib_consec < VIB_CONSEC) {
        return false;                   /* 还没连续够次数，可能是单点尖峰 */
    }

    /* 冷却检查：一次晃动会持续好几轮，只报第一次 */
    int64_t t = now_us();
    if (s_vib_last_report_us != 0 &&
        (t - s_vib_last_report_us) < (int64_t)VIB_COOLDOWN_MS * 1000) {
        return false;
    }
    s_vib_last_report_us = t;
    s_vib_consec = 0;                   /* 报过就重新数，下一轮冷却结束才可能再报 */

    if (lock()) {
        s_snap.vib_count++;
        unlock();
    }

    /* 震动是**事件**，不是周期读数，所以在这里就地打印，
     * 不并进统一状态行 —— 否则要等下一个打印周期才知道被碰过。
     * 行首统一加 "EVT" 前缀，方便和周期行区分。 */
    ESP_LOGW(TAG, "EVT vibration: mag deviation %.0f mg (thresh %d mg) "
                  "accel=(%.2f, %.2f, %.2f) g",
             dev, VIB_THRESH_MG, d.ax, d.ay, d.az);
    return true;
}

/* PIR：读原始电平 -> 维护保持窗口 -> 决定存在状态。
 * 返回 true 表示 presence 状态发生了变化。 */
static bool sample_pir(void)
{
    if (!pir_ready()) {
        return false;
    }

    bool raw = pir_read();
    int64_t t = now_us();
    bool changed = false;

    if (raw) {
        s_pir_last_seen_us = t;
    }

    /* 保持窗口：raw 为高直接算存在；raw 掉低但还在窗口内也算存在。
     * PIR 模块自身有 2-3s 再触发窗口，中途电平会抖，所以需要这层。 */
    bool presence = false;
    if (raw) {
        presence = true;
    } else if (s_pir_last_seen_us != 0 &&
               (t - s_pir_last_seen_us) < (int64_t)PIR_HOLD_MS * 1000) {
        presence = true;
    }

    if (presence != s_presence) {
        changed = true;
        s_presence = presence;
    }

    /* 原始电平**每变一次**就打一行（上/下沿都打）。
     *
     * 这是排查"pir 读数一直不变"的关键证据，必须打 raw 而不是 presence：
     *   - 从头到尾没有 "EVT pir raw" 行 -> 引脚电平压根没动过，
     *     模块没输出（供电/接线/极性问题），应用层再怎么调都没用；
     *   - 有 raw 行但状态行里的数字不变 -> 是保持窗口或再触发窗口的问题。
     * 早期版本只在 raw 上升沿且灯态变化时打日志，模块 5V 供电不足时
     * OUT 恒定高电平，日志里什么都看不到，白白怀疑了半天的应用层逻辑。 */
    if (raw != s_pir_raw_prev) {
        ESP_LOGI(TAG, "EVT pir raw: %d -> %d (presence %d)",
                 s_pir_raw_prev ? 1 : 0, raw ? 1 : 0, presence ? 1 : 0);
    }
    s_pir_raw_prev = raw;

    if (lock()) {
        s_snap.pir_ok = true;
        s_snap.presence = presence;
        s_snap.pir_raw = raw;
        unlock();
    }

#if CONFIG_APP_PIR_LAMP_ENABLE
    /* --- PIR 控灯：raw 高电平期间一直绿闪，低电平停 ---
     *
     * 规则：看 raw 的**电平**（不是边沿），并只在电平翻转时动灯。
     *
     * 为什么用"很长的 total_ms"来实现"一直闪"，而不是每 tick 重调：
     *   lamp_indicator_start() 的幂等去重比的是颜色+on/off 节奏，**不含
     *   total_ms**；而且到期由渲染任务把 s_ind_active 清掉。若每 tick 都
     *   重调，一旦真正的 total_ms 到期就重开，相位从 0 重算，闪烁节奏会
     *   周期性"卡一下"；同时每次到期/重开都各打一行日志，几秒一条刷屏。
     *   所以这里只调两次（高/低各一次），用一个长到不可能提前到期的
     *   total_ms，让指示器在"有人"期间一直有效、相位连续。
     *
     * 为什么只在电平翻转时动灯、不在每 tick 无脑调：
     *   每 tick 调虽然被去重挡住不重置计时，但每次调用都要抢 lamp 的锁；
     *   而且 raw 抖动时会在 0/1 之间来回，电平翻转判定天然把它挡在外面
     *   （真正的停/启只在真的翻转时发生）。
     *
     * 注意 raw 会抖：PIR 模块在有人期间 raw 会高低跳变（这也是保持窗口
     * PIR_HOLD_MS 存在的原因）。所以人是"动着"时灯会断续，人静止时
     * 若模块 raw 掉低，灯也会停 —— 这是 raw 电平控制的固有现象，不是
     * 卡死；想让"人走后还多亮一会儿"应改用 presence。
     *
     * 不碰 s_pir_lamp_lit / s_pir_lamp_latched：闪烁不改变灯的开关状态，
     * 那两个标志归"PIR 点亮灯"逻辑，误置会污染云端优先的锁存。
     *
     * 与闹钟/告警闪红的区别：那是红色 (255,0,0)，这里是绿色 (0,255,0)。 */
    if (raw && !s_pir_blinking) {
        s_pir_blinking = true;
        lamp_indicator_start(0, 255, 0, PIR_BLINK_ON_MS, PIR_BLINK_OFF_MS,
                             PIR_BLINK_FOREVER_MS);
        ESP_LOGI(TAG, "EVT pir: raw high -> lamp blink start");
    } else if (!raw && s_pir_blinking) {
        s_pir_blinking = false;
        lamp_indicator_stop();
        ESP_LOGI(TAG, "EVT pir: raw low -> lamp blink stop");
    }
#endif

    return changed;
}

/* HC-SR04：读一次距离。
 *
 * 这里**不打印** —— 所有传感器的读数由 env_task() 末尾的统一状态行
 * 一次性输出（见 env_print_status），保证串口上每个周期只有一行。
 * 读失败时快照里 dist_ok=false，统一行会显示 "--"，而不是残留的旧值；
 * 否则串口看上去"一切正常"，实际模块可能已经掉了。 */
static void sample_distance(void)
{
    if (!hcsr04_ready()) {
        return;
    }

    float cm = 0.0f;
    bool ok = hcsr04_measure_cm(&cm, CONFIG_APP_HCSR04_TIMEOUT_US);

    if (lock()) {
        s_snap.dist_ok = ok;
        if (ok) {
            s_snap.distance_cm = cm;
        }
        unlock();
    }
}

/* SHT30：读一次温湿度。
 *
 * 驱动是 I2C 器件，理论上可以每 tick 读，但它和 DHT22 测的是同一个环境，
 * 读那么快没有意义（DHT22 器件下限 2s），所以与 DHT22 同频。
 * sht30_read() 内部有自己的互斥，和 rest_api 上报任务并发调用是安全的。 */
static void sample_sht30(void)
{
    float t = 0.0f, h = 0.0f;
    bool ok = sht30_read(&t, &h);

    if (lock()) {
        s_snap.sht30_ok = ok;
        if (ok) {
            s_snap.sht30_temp_c = t;
            s_snap.sht30_hum_pct = h;
        }
        unlock();
    }
}

/* DHT22：读一次温湿度（驱动内部已做 2s 节流） */
static void sample_dht(void)
{
    if (!dht22_ready()) {
        return;
    }

    float t = 0.0f, h = 0.0f;
    bool ok = dht22_read(&t, &h);

    if (lock()) {
        s_snap.dht_ok = ok;
        if (ok) {
            s_snap.dht_temp_c = t;
            s_snap.dht_hum_pct = h;
        }
        unlock();
    }
}

/* ------------------------------------------------------------------ */
/* 统一状态行                                                          */
/* ------------------------------------------------------------------ */

/* 把所有传感器读数打成一行，例如：
 *   SHT30 : tem 24.3 hum 51.2, DHT22 : TEM 24.1 hum 50.8, pir : 0,
 *   HC-SR04 : distance 132cm, mpu6050 : a 0.02/-0.01/1.00 g,
 *   g 0.1/-0.3/0.2 dps, t 31.2 C, dev 5
 *
 * 格式是上位机/人一起看的，所以字段名固定、不留空：读不到的传感器只把
 * **自己那一段**打成一串 '-'（如 "DHT22 : TEM -- hum --"），而不是整行省略，
 * 这样"哪个传感器掉了"一眼可见，字段位置也不会错位。
 * pir 打的是模块 OUT 引脚的原始电平（读 IO 口，无保持），与上报用的
 * presence 不是同一个量，详见下面 pir 那段。
 *
 * 这是**唯一**的周期性传感器输出 —— 各 sample_*() 都不打印，
 * 由采集任务按 PRINT_MS 调这里一次，串口上每个周期只有一行，
 * 而不是"每路传感器各打各的、互相穿插"。
 *
 * 只有确实处于告警时才追加 ALERT 尾部，正常时行更短、更好扫读。 */
static void env_print_status(void)
{
    env_snapshot_t s;
    env_get_snapshot(&s);

    /* 每段都先拼好再一次性输出：单条 ESP_LOGI 是原子的，
     * 分开打会被其它任务的日志插到中间。
     *
     * 注意**不要**用 "%s" 二次拷贝另一个 snprintf 缓冲区：
     * gcc 按"源缓冲可能有的最大长度"推算截断风险，即使实际内容
     * 只有几个字符也会报 -Werror=format-truncation。直接在本缓冲区里
     * 一次格式化，目标够大就不会有告警。 */
    char sht[64];
    if (s.sht30_ok) {
        snprintf(sht, sizeof(sht), "tem %.1f hum %.1f",
                 s.sht30_temp_c, s.sht30_hum_pct);
    } else {
        snprintf(sht, sizeof(sht), "tem -- hum --");
    }

    char dht[64];
    if (s.dht_ok) {
        snprintf(dht, sizeof(dht), "TEM %.1f hum %.1f",
                 s.dht_temp_c, s.dht_hum_pct);
    } else {
        snprintf(dht, sizeof(dht), "TEM -- hum --");
    }

    /* PIR：打模块 OUT 引脚的**原始电平**，就是读一下 IO 口，不做任何保持。
     *
     * 它是硬件事实，不是业务结论：本行只回答"模块此刻有没有输出"。
     * 判断接线/供电有没有问题时看这个字段就够了 ——
     *   raw 恒 0（模块完全不输出）、raw 恒 1（输出钉死在高电平，
     *   HC-SR501 跳线选错或供电不足时自锁）都是模块侧的问题。
     *
     * 注意"有人"这个业务结论不在这里，而是 presence（按 PIR_HOLD_MS 保持
     * 过的存在状态）。两者会不一致且**属于正常现象**：raw 是瞬时值，模块
     * 自身有再触发窗口，人站着不动时 raw 会掉低，presence 仍在窗口内保持为
     * 1。上报云端、灯控用的都是 presence，所以本行显示 0 不代表云端认为
     * 没人。要排查保持窗口就对照 sample_pir() 的 "EVT pir raw: x -> y" 行。 */
    char pir[16];
    if (s.pir_ok) {
        snprintf(pir, sizeof(pir), "%d",
                 s.pir_raw ? 1 : 0);
    } else {
        snprintf(pir, sizeof(pir), "--");
    }

    char dist[16];
    if (s.dist_ok) {
        snprintf(dist, sizeof(dist), "%.0fcm", s.distance_cm);
    } else {
        snprintf(dist, sizeof(dist), "--");
    }

    /* MPU6050：把六轴 + 温度全打出来，不再只给一个合加速度偏差。
     *
     * 排布成 "a x/y/z" + "g x/y/z" + "t" 三段，是为了让同一根轴在不同行
     * 之间**纵向对齐**，扫一眼就能看出哪个轴在动（调试接线和安装方向时
     * 最常用的就是这个）。轴向约定：芯片平放 az≈1g；绕某个轴转，对应的
     * 那一路 g 值变化最明显。
     *
     * 末尾仍保留 "dev"（合加速度与 1g 的偏差，震动判据用的就是它），
     * 这样"为什么报/不报震动"能对着同一行直接看。 */
    char imu[128];
    if (s.imu_ok) {
        snprintf(imu, sizeof(imu),
                 "a %.2f/%.2f/%.2f g, g %.1f/%.1f/%.1f dps, t %.1f C, dev %.0f",
                 s.imu_ax, s.imu_ay, s.imu_az,
                 s.imu_gx, s.imu_gy, s.imu_gz,
                 s.imu_temp_c, s.accel_dev_mg);
    } else {
        snprintf(imu, sizeof(imu), "--");
    }

    if (s.alerts != 0) {
        ESP_LOGW(TAG, "SHT30 : %s, DHT22 : %s, pir : %s, HC-SR04 : distance "
                      "%s, mpu6050 : %s | ALERT %s",
                 sht, dht, pir, dist, imu, env_alert_name(s.alerts));
    } else {
        ESP_LOGI(TAG, "SHT30 : %s, DHT22 : %s, pir : %s, HC-SR04 : distance "
                      "%s, mpu6050 : %s",
                 sht, dht, pir, dist, imu);
    }
}

/* ------------------------------------------------------------------ */
/* 采集任务                                                            */
/* ------------------------------------------------------------------ */

static void env_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;

    ESP_LOGI(TAG, "task start: tick=%dms (mpu=1, pir=2, dist=10, dht/sht30=40 "
                  "ticks), status line every %d ms",
             TICK_MS, PRINT_MS);

    for (;;) {
        bool need_report = false;
        bool need_alert_check = false;

        /* --- MPU6050 --- */
        if ((tick % MPU_EVERY_TICKS) == 0) {
            if (sample_mpu()) {
                need_report = true;      /* 震动 -> 立即上报 */
            }
            need_alert_check = true;
        }

        /* --- PIR --- */
        if ((tick % PIR_EVERY_TICKS) == 0) {
            if (sample_pir()) {
                need_report = true;      /* 存在状态翻转 -> 立即上报 */
            }
        }

        /* --- HC-SR04 --- */
        if ((tick % DIST_EVERY_TICKS) == 0) {
            sample_distance();
            need_alert_check = true;
        }

        /* --- DHT22 --- */
        if ((tick % DHT_EVERY_TICKS) == 0) {
            sample_dht();
            need_alert_check = true;
        }

        /* --- SHT30 --- */
        if ((tick % SHT30_EVERY_TICKS) == 0) {
            sample_sht30();
        }

        /* --- 告警判定 ---
         * 只在有新的温湿度/距离数据时才重算，避免每 50ms 空转。 */
        if (need_alert_check && lock()) {
            bool alert_changed = recompute_alerts_locked();
            uint32_t alerts = s_snap.alerts;
            unlock();

            if (alert_changed) {
                if (alerts != 0) {
                    ESP_LOGW(TAG, "EVT alert: %s (0x%02X) - WS2812 blinking red",
                             env_alert_name(alerts), (unsigned)alerts);
                    /* 离线也照常闪 —— 告警不依赖网络 */
                    lamp_indicator_start(255, 0, 0,
                                         ALERT_BLINK_ON_MS, ALERT_BLINK_OFF_MS,
                                         ALERT_BLINK_TOTAL_MS);
                } else {
                    ESP_LOGI(TAG, "EVT alert: cleared");
                    lamp_restore();
                }
                need_report = true;      /* 告警翻转 -> 立即补报 */
            }
        }

        /* --- 统一状态行 ---
         * 放在告警判定之后，这样行尾的 ALERT 与本行读数严格对应，
         * 不会出现"先打旧读数、再打新告警"的错位。
         * PRINT_EVERY_TICKS 为 0（PRINT_MS=0）时整段被优化掉。 */
#if PRINT_EVERY_TICKS > 0
        if (tick != 0 && (tick % PRINT_EVERY_TICKS) == 0) {
            env_print_status();
        }
#endif

        /* --- 事件驱动上报 ---
         * rest_api_request_report() 是置事件位唤醒上报任务，**非阻塞**，
         * 不会在这里同步发 HTTP（那会阻塞采集任务好几秒）。 */
        if (need_report) {
            rest_api_request_report();
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ------------------------------------------------------------------ */
/* 对外的初始化与查询                                                    */
/* ------------------------------------------------------------------ */

esp_err_t env_sensors_init(void)
{
#if !CONFIG_APP_ENV_TASK_ENABLE
    ESP_LOGW(TAG, "sensor task disabled (CONFIG_APP_ENV_TASK_ENABLE=n)");
    return ESP_OK;
#else
    if (s_started) {
        return ESP_OK;                  /* 幂等 */
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* --- MPU6050：独占自己的 I2C 总线 ---
     * 它和 SHT30 挂在**不同的引脚**上（本工程默认 SHT30 在 GPIO12/13、
     * MPU6050 在 GPIO15/16），所以不能像以前那样借 sht30_get_bus()：
     * 借来的总线上根本没有 MPU6050，只会得到一句容易被忽略的 "not found"，
     * 表现为状态行里永远是 "imu --(>=300) vib 0"。
     * 端口用 I2C_NUM_0（SHT30 占着 I2C_NUM_1），由驱动自己创建和销毁。
     *
     * 引脚**不要**选 GPIO41/42：ESP32-S3 上那是 USB 的 D-/D+，而本工程
     * 开着 USB Serial/JTAG 二级控制台，I2C 挂上去会和 USB 抢同一对脚 ——
     * 症状是开机能读、跑几十秒后开始 NACK，两个传感器一起时好时坏。 */
    if (mpu6050_bus_init(CONFIG_APP_MPU6050_SDA_GPIO,
                         CONFIG_APP_MPU6050_SCL_GPIO) != ESP_OK) {
        ESP_LOGW(TAG, "MPU6050 I2C bus init failed (SDA=GPIO%d SCL=GPIO%d) - "
                      "vibration detection disabled",
                 CONFIG_APP_MPU6050_SDA_GPIO, CONFIG_APP_MPU6050_SCL_GPIO);
    } else if (mpu6050_init(NULL, CONFIG_APP_MPU6050_ADDR) == ESP_OK) {
        if (mpu6050_set_accel_range(CONFIG_APP_MPU6050_ACCEL_RANGE) != ESP_OK) {
            ESP_LOGW(TAG, "MPU6050 accel range set failed - using default");
        }
        ESP_LOGI(TAG, "MPU6050 ready (SDA=GPIO%d SCL=GPIO%d addr=0x%02X, "
                      "vibration thresh %d mg, %d consec, cooldown %d ms)",
                 CONFIG_APP_MPU6050_SDA_GPIO, CONFIG_APP_MPU6050_SCL_GPIO,
                 CONFIG_APP_MPU6050_ADDR,
                 VIB_THRESH_MG, VIB_CONSEC, VIB_COOLDOWN_MS);
    } else {
        ESP_LOGW(TAG, "MPU6050 not found on I2C0 (SDA=GPIO%d SCL=GPIO%d) - "
                      "vibration detection disabled; check wiring, the 4.7k "
                      "pull-ups and the address (AD0 low=0x68, high=0x69, "
                      "currently configured 0x%02X)",
                 CONFIG_APP_MPU6050_SDA_GPIO, CONFIG_APP_MPU6050_SCL_GPIO,
                 CONFIG_APP_MPU6050_ADDR);
    }

    /* --- DHT22 --- */
    if (dht22_init(CONFIG_APP_DHT22_GPIO) == ESP_OK) {
        ESP_LOGI(TAG, "DHT22 ready (GPIO%d)", CONFIG_APP_DHT22_GPIO);
    } else {
        ESP_LOGW(TAG, "DHT22 init failed (GPIO%d)", CONFIG_APP_DHT22_GPIO);
    }

    /* --- HC-SR04 --- */
    if (hcsr04_init(CONFIG_APP_HCSR04_TRIG_GPIO,
                    CONFIG_APP_HCSR04_ECHO_GPIO) == ESP_OK) {
        ESP_LOGI(TAG, "HC-SR04 ready (TRIG=GPIO%d ECHO=GPIO%d, alert < %d cm)",
                 CONFIG_APP_HCSR04_TRIG_GPIO, CONFIG_APP_HCSR04_ECHO_GPIO,
                 ALERT_DIST_MIN);
    } else {
        ESP_LOGW(TAG, "HC-SR04 init failed (TRIG=GPIO%d ECHO=GPIO%d)",
                 CONFIG_APP_HCSR04_TRIG_GPIO, CONFIG_APP_HCSR04_ECHO_GPIO);
    }

    /* --- PIR --- */
    if (pir_init(CONFIG_APP_PIR_GPIO) == ESP_OK) {
        ESP_LOGI(TAG, "PIR ready (GPIO%d, hold %d ms, blink hint %s)",
                 CONFIG_APP_PIR_GPIO, PIR_HOLD_MS,
#if CONFIG_APP_PIR_LAMP_ENABLE
                 "on");
#else
                 "off");
#endif
    } else {
        ESP_LOGW(TAG, "PIR init failed (GPIO%d)", CONFIG_APP_PIR_GPIO);
    }

    if (xTaskCreate(env_task, "env_sensors", 4096, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
#endif
}

void env_get_snapshot(env_snapshot_t *out)
{
    if (!out) {
        return;
    }
    if (lock()) {
        *out = s_snap;
        unlock();
    } else {
        memset(out, 0, sizeof(*out));
    }
}

void env_dump(void)
{
    env_print_status();
}

/* ------------------------------------------------------------------ */
/* 与灯的协同：PIR 只提示，不改灯态                                       */
/* ------------------------------------------------------------------ */

/* 由 rest_api.c 在收到云端 lamp 命令时调用。
 *
 * 现在是个空钩子。它原本负责"云端优先"：云端关灯时给 PIR 上锁（免得
 * PIR 下一轮又点灯）、云端开灯时清掉"PIR 点亮"标记。但 PIR 改成只做
 * 绿闪提示、不再 lamp_set() 之后，那两个标志已无人读取，这套锁存逻辑
 * 失去了意义 —— 保留下来只会让日志声称"PIR latched until presence
 * ends"却什么也没锁住，属于会误导排查的死逻辑，故清空。
 *
 * 钩子本身留着：调用点（rest_api.c / shadow.c）与 lamp_set() 成对出现，
 * 日后若要给 PIR 加回开灯行为，这里就是唯一的挂载处。 */
void env_notify_lamp_command(bool on)
{
    (void)on;
}
