/* console.c - 最小串口命令行（详见 console.h）
 *
 * 命令一览（在串口里敲完回车）：
 *   help                                打印帮助
 *   time                                打印当前时间（与 APP_TIME_LOG_S 的周期日志同格式）
 *   time set <epoch>                    强制设定时间（离线测试用，标记为 nvs 来源）
 *   alarm                               列出 4 个槽位
 *   alarm set <slot> <HH:MM> <days> [dur]
 *                                       days 可以是 all 或 0-6 的逗号列表（0=周日）
 *                                       例：alarm set 0 07:30 1,2,3,4,5 10
 *   alarm off <slot>                    停用某槽
 *   ring [sec]                          立即试响（默认 5 秒，不落盘、不参与去重）
 *   wifi                                网络与校时状态
 *   shadow                              后端与最近一次指令结果
 *   reboot                              重启
 *
 * 全部命令都是只读或显式修改，不做任何隐式动作 —— 调试台崩了不能影响
 * 闹钟和上报，所以整个循环里没有任何会 abort 的调用。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "console.h"
#include "app_config.h"
#include "app_version.h"
#include "time_sync.h"
#include "alarm.h"
#include "lamp.h"
#include "wifi_net.h"
#include "aws_iot.h"
#include "shadow.h"

static const char *TAG = "console";

/* 512 而不是 96：delta 命令后面跟的是整段 JSON（带空格），
 * 96 字节连一条"闹钟全参数"的 desired 都放不下，用户稍微写全一点
 * 就会被 fgets 切成两半，后半截残留在缓冲里污染下一条命令 ——
 * 症状是 unknown command "..." 里出现半截 JSON、或 delta 报
 * "JSON parse failed (N bytes)" 且 N 明显偏小。 */
#define LINE_MAX 512

/* 所有 strtok 必须用**同一套**分隔符，包括 '\r' '\n'。
 * 踩过的坑：主循环用 " \t\r\n" 取命令，子命令却用 " \t" 取参数，
 * 于是 "wifi off" 的第二个 token 是 "off\r\n" —— strcmp 跟 "off" 不相等，
 * 分支静默走错（表现为"发了 wifi off，回的是 wifi 状态"）。
 * 命令名恰好没踩到只是因为 strcmp 之前先比对了前缀。 */
#define DELIM " \t\r\n"

static void print_help(void)
{
    printf("\n"
           "--- serial console (fw %s) ---\n"
           "  help\n"
           "  time                               show clock\n"
           "  time set <epoch>                   force-set clock (offline test)\n"
           "  alarm                              list slots\n"
           "  alarm set <slot> <HH:MM> <days> [dur]\n"
           "                                     days = all | 0..6 list (0=Sun)\n"
           "  alarm off <slot>                   disable a slot\n"
           "  ring [sec]                         fire a test ring now (default 5)\n"
           "  wifi / wifi off / wifi on           status / simulate NO NETWORK\n"
           "  shadow / reboot\n"
           "-------------------------------\n\n", APP_FW_VERSION);
}

static void print_time(void)
{
    time_t now = time(NULL);
    char iso[32];
    time_sync_iso(now, iso, sizeof(iso));
    printf("TIME epoch=%lld iso=%s src=%s synced=%d valid=%d tz=%s\n",
           (long long)now, iso, time_sync_src_str(),
           (int)time_sync_is_synced(), (int)time_sync_is_valid(), APP_TZ);
}

/* "1,2,3,4,5" 或 "all" -> 位图（bit0 = 周日，与 struct tm 的 tm_wday 对齐） */
static bool parse_days(const char *s, uint8_t *out)
{
    if (strcmp(s, "all") == 0 || strcmp(s, "*") == 0) {
        *out = ALARM_DAY_ALL;
        return true;
    }
    uint8_t days = 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int d = atoi(tok);
        if (d < 0 || d > 6) {
            return false;
        }
        days |= (uint8_t)(1u << d);
    }
    if (days == 0) {
        return false;
    }
    *out = days;
    return true;
}

static void print_days(uint8_t days)
{
    static const char *n[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    bool first = true;
    for (int d = 0; d < 7; d++) {
        if (days & (1u << d)) {
            printf("%s%s", first ? "" : ",", n[d]);
            first = false;
        }
    }
}

static void print_alarms(void)
{
    time_t nf = alarm_next_fire();
    char iso[32] = "-";
    if (nf) {
        time_sync_iso(nf, iso, sizeof(iso));
    }
    printf("alarm slots=%d state=%s next=%lld (%s) last=%lld ringing_left=%ds\n",
           ALARM_SLOTS, alarm_state_str(), (long long)nf, iso,
           (long long)alarm_last_fire(), alarm_ringing_remaining_s());
    for (int i = 0; i < ALARM_SLOTS; i++) {
        const alarm_t *a = alarm_get(i);
        printf("  [%d] %-3s id=%u %02u:%02u dur=%us days=",
               i, a->enable ? "ON" : "off", (unsigned)a->id,
               (unsigned)a->hour, (unsigned)a->minute, (unsigned)a->duration_s);
        print_days(a->days);
        printf("\n");
    }
}

/* alarm set <slot> <HH:MM> <days> [dur] */
static void cmd_alarm_set(char *args)
{
    char *slot_s = strtok(args, DELIM);
    char *hm     = strtok(NULL, DELIM);
    char *days_s = strtok(NULL, DELIM);
    char *dur_s  = strtok(NULL, DELIM);

    if (!slot_s || !hm || !days_s) {
        printf("usage: alarm set <slot> <HH:MM> <days> [dur]\n");
        return;
    }
    int slot = atoi(slot_s);
    if (slot < 0 || slot >= ALARM_SLOTS) {
        printf("slot must be 0..%d\n", ALARM_SLOTS - 1);
        return;
    }
    int hh = -1, mm = -1;
    if (sscanf(hm, "%d:%d", &hh, &mm) != 2) {
        printf("time must be HH:MM\n");
        return;
    }
    uint8_t days = 0;
    if (!parse_days(days_s, &days)) {
        printf("days must be 'all' or a 0..6 list (0=Sun), e.g. 1,2,3,4,5\n");
        return;
    }
    int dur = dur_s ? atoi(dur_s) : APP_ALARM_DURATION_S;

    const alarm_t *cur = alarm_get(slot);
    alarm_t a = {
        .id = cur->id,
        .enable = true,
        .hour = (uint8_t)hh,
        .minute = (uint8_t)mm,
        .days = days,
        .duration_s = (uint16_t)dur,
    };
    if (alarm_set(slot, &a) != ESP_OK) {
        printf("rejected (hour 0-23, minute 0-59, dur 1-3600)\n");
        return;
    }
    print_alarms();
}

static void cmd_alarm(char *args)
{
    char *sub = strtok(args, DELIM);
    if (!sub) {
        print_alarms();
        return;
    }
    if (strcmp(sub, "set") == 0) {
        cmd_alarm_set(NULL);       /* 后续 token 在同一个 strtok 上下文里 */
        return;
    }
    if (strcmp(sub, "off") == 0) {
        char *slot_s = strtok(NULL, DELIM);
        if (!slot_s) {
            printf("usage: alarm off <slot>\n");
            return;
        }
        int slot = atoi(slot_s);
        if (alarm_clear(slot) != ESP_OK) {
            printf("slot must be 0..%d\n", ALARM_SLOTS - 1);
            return;
        }
        print_alarms();
        return;
    }
    printf("usage: alarm | alarm set ... | alarm off <slot>\n");
}

static void cmd_time(char *args)
{
    char *sub = strtok(args, DELIM);
    if (!sub) {
        print_time();
        return;
    }
    if (strcmp(sub, "set") == 0) {
        char *e = strtok(NULL, DELIM);
        if (!e) {
            printf("usage: time set <epoch>\n");
            return;
        }
        long long v = atoll(e);
        if (time_sync_set_epoch((time_t)v, TIME_SRC_NVS) != ESP_OK) {
            printf("rejected: epoch must be >= %ld for the time to be trusted\n",
                   (long)APP_TIME_MIN_VALID);
            return;
        }
        /* 手动设定的时间也要落盘，否则重启就丢 */
        time_sync_save_now();
        print_time();
        return;
    }
    printf("usage: time | time set <epoch>\n");
}

static void cmd_wifi(char *args)
{
    char *sub = strtok(args, DELIM);
    if (sub && (strcmp(sub, "off") == 0 || strcmp(sub, "on") == 0)) {
        bool off = (strcmp(sub, "off") == 0);
        net_sta_suppress(off);
        if (off) {
            printf("WiFi disabled. The device is as good as unplugged from the "
                   "network:\n"
                   "  - clock keeps running on the local RTC (see 'time')\n"
                   "  - alarms still fire (see 'ring' / 'alarm')\n"
                   "  - shadow topics stay unsubscribed until 'wifi on'\n");
        } else {
            printf("WiFi re-enabled, reconnecting...\n");
        }
        return;
    }
    printf("wifi: connected=%d rssi=%d dBm suppressed=%d sntp=%s synced=%d\n",
           (int)net_sta_is_connected(), net_sta_rssi(),
           (int)net_sta_is_suppressed(),
           time_sync_src_str(), (int)time_sync_is_synced());
}

static void cmd_shadow(void)
{
    printf("backend=%s (APP_BACKEND=%d) shadow=%d mqtt=%d thing=%s "
           "interval=%ds cmd_result=%s\n",
           (APP_BACKEND == 1) ? "AWS" : "REST", (int)APP_BACKEND,
           (int)APP_SHADOW_ENABLE, (int)aws_iot_is_connected(), aws_iot_thing(),
           shadow_report_interval_s(), shadow_last_cmd_result());
    if (!aws_iot_is_connected()) {
        printf("  mqtt not connected -> offline (shadow topics not subscribed)\n");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX];

    print_help();

    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            /* stdin 被关闭或读错误时不要空转，歇一下再来 */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* fgets 读满 LINE_MAX-1 字节就返回（且不含行尾），剩下的字节会
         * 被下一次 fgets 当成另一条新命令读进来 —— 表现为一串莫名其妙的
         * unknown command，最后一条才是"看起来像真的"的半截 JSON。
         * 与其让它静默切片，不如把这一行的剩余部分丢掉并明确报错。 */
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = fgetc(stdin)) != '\n' && ch != EOF) {
                /* 丢弃超长行的剩余字节 */
            }
            printf("line too long (max %d), dropped\n", LINE_MAX - 1);
            continue;
        }

        /* 手工把"命令名"和"剩余原文"切开，而不是一路 strtok：
         * delta 命令后面跟的是含空格的 JSON，被 strtok 切碎就没法用了。
         * 切的位置是空格处，所以 args 里仍然保留着引号与空格。 */
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') {
            cmd++;
        }
        char *args = cmd;
        while (*args && *args != ' ' && *args != '\t' && *args != '\r' && *args != '\n') {
            args++;
        }
        if (*args) {
            *args++ = '\0';
        }
        while (*args == ' ' || *args == '\t') {
            args++;
        }
        if (cmd[0] == '\0') {
            continue;
        }

        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            print_help();
        } else if (strcmp(cmd, "time") == 0) {
            cmd_time(args);
        } else if (strcmp(cmd, "alarm") == 0) {
            cmd_alarm(args);
        } else if (strcmp(cmd, "ring") == 0) {
            int sec = args[0] ? atoi(args) : 5;
            if (alarm_test_fire(sec) != ESP_OK) {
                printf("seconds must be 1..600\n");
            }
        } else if (strcmp(cmd, "delta") == 0) {
            if (args[0] == '\0') {
                printf("usage: delta {\"state\":{\"desired\":{\"lamp\":\"ON\"}}}\n");
            } else {
                shadow_inject_delta(args);
            }
        } else if (strcmp(cmd, "wifi") == 0) {
            cmd_wifi(args);
        } else if (strcmp(cmd, "shadow") == 0) {
            cmd_shadow();
        } else if (strcmp(cmd, "reboot") == 0) {
            printf("rebooting...\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else {
            printf("unknown command \"%s\" (try: help)\n", cmd);
        }
    }
}

esp_err_t console_start(void)
{
    static bool started = false;
    if (started) {
        return ESP_OK;
    }
    if (xTaskCreate(console_task, "console", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    started = true;
    ESP_LOGI(TAG, "serial console ready - type 'help'");
    return ESP_OK;
}
