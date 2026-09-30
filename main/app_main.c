/* AWS_mqtt_REST_API - SHT30 -> ESP32-S3 -> WiFi -> (AWS IoT Core | REST API)
 *
 *   SHT30 --I2C--> ESP32 --WiFi + MQTT over TLS(8883)--> AWS IoT Core
 *                        (根CA + 设备证书 + 私钥，双向认证)
 *                      \-- WiFi + HTTP(S) POST/GET --> 自己的 REST 服务器
 *
 * 启动流程（顺序是有意的，改动前先看下面的说明）：
 *   1. NVS 配置 + netif/事件循环/WiFi 驱动初始化
 *   2. **恢复时间**（NVS 里的 epoch）+ 设置时区
 *   3. 传感器、灯
 *   4. **启动离线闹钟引擎** —— 必须在任何网络动作之前
 *   5. 没有 WiFi 配置 -> 进 SoftAP 配网页（阻塞在这里）
 *      有配置但连不上 -> **离线模式**：继续往下跑，不进配网
 *   6. 联网了 -> 启动 SNTP 校时 + 上电版本检查
 *   7. 按 APP_BACKEND 启动上报通道（AWS Shadow / REST），并起保活与配网按键任务
 *
 * 为什么第 4 步在网络之前：闹钟是本地功能，如果它依赖 WiFi 连上才启动，
 * 那"断网仍能响"就无从谈起。WiFi 连不上时设备**照常走时、照常响铃**，
 * MQTT 只是在后台不停重连，连上了自动补报期间发生的一切。
 *
 * 为什么"连不上不进配网"：早期版本连不上就直接 run_provisioning_ap()，
 * 于是拔掉路由器重启后设备变成配网热点，闹钟彻底不工作 —— 与"离线可用"
 * 的需求直接冲突。现在只有**从未配过网**才进配网；已配过网的设备即使
 * 路由器长期不在，也老老实实离线运行。要重新配网请长按 GPIO0 三秒。
 *
 * 配置在哪里改：
 *   - WiFi 名称/密码：配网页（SoftAP 热点 -> 192.168.4.1）。
 *   - 后端选择、REST 服务器地址、版本号、时区、闹钟参数：
 *     main/app_config.h 的宏，改完重新编译烧录。
 *   - AWS 端点 / Thing 名 / 三份 PEM 证书：main/aws_certs.h 的宏。
 *
 * 重新配网：按住 BOOT 键（GPIO0）3 秒，清空 WiFi 配置并重启进入配网模式。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "app_cfg.h"
#include "app_version.h"
#include "aws_certs.h"
#include "wifi_net.h"
#include "webprov.h"
#include "sht30.h"
#include "lamp.h"
#include "env_sensors.h"
#include "aws_iot.h"
#include "rest_api.h"
#include "ota_update.h"
#include "time_sync.h"
#include "alarm.h"
#include "shadow.h"
#include "console.h"

static const char *TAG = "app";

#define REPROVISION_HOLD_MS 3000

/* 生成配网热点名（带 MAC 尾号）：ESP32S3-SHT30-XXXXXXXXXXXX */
static void make_ap_ssid(char *buf, size_t sz)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, sz, "%s-%02X%02X%02X%02X%02X%02X", CONFIG_APP_AP_SSID_PREFIX,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void gpio_input_pullup(int pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* 进入配网模式：起热点 + 网页，然后停在这里（httpd 在后台线程处理请求）。
 * 保存配置的 handler 会 esp_restart()，所以不会走到别处。
 *
 * 注意：这里**只有**从未配过网的设备会进来。闹钟引擎已经在 app_main 里
 * 起好了，配网期间照常走时、照常响铃。 */
static void run_provisioning_ap(const char *why)
{
    char ssid[48];
    make_ap_ssid(ssid, sizeof(ssid));

    ESP_LOGW(TAG, "entering provisioning mode (%s)", why);
    net_ap_start(ssid, CONFIG_APP_AP_PASSWORD);
    webprov_start();

    ESP_LOGI(TAG, "connect to AP \"%s\" (pass \"%s\") and open http://192.168.4.1",
             ssid, CONFIG_APP_AP_PASSWORD);

    for (;;) {
        printf("ESP32S3_SN:%s\n", ssid);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* 配网按键任务：长按 3s 清配置重启进入配网模式 */
static void reprovision_btn_task(void *arg)
{
    (void)arg;
    int pin = CONFIG_APP_PTT_GPIO;
    const TickType_t hold_reset = pdMS_TO_TICKS(REPROVISION_HOLD_MS);
    bool was_pressed = false;
    bool reset_triggered = false;
    TickType_t press_start = 0;

    gpio_input_pullup(pin);

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool pressed = (gpio_get_level(pin) == 0);

        if (pressed) {
            if (!was_pressed) {
                press_start = now;
                reset_triggered = false;
            }
            was_pressed = true;

            if (!reset_triggered && (now - press_start) >= hold_reset) {
                reset_triggered = true;
                ESP_LOGW(TAG, "long press %d ms - clearing config and rebooting "
                         "into provisioning mode", REPROVISION_HOLD_MS);
                vTaskDelay(pdMS_TO_TICKS(300));
                cfg_erase();

                /* GPIO0 是 ESP32-S3 的 strapping 引脚：复位时若仍被拉低，
                 * 芯片会进入串口下载模式而不是运行固件，所以先等松手。 */
                for (int i = 0; i < 1000 && gpio_get_level(pin) == 0; i++) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                ESP_LOGW(TAG, "button released - rebooting into provisioning");
                esp_restart();
            }
        } else {
            was_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* WiFi 保活 + SNTP 兜底启动。
 * 设备可能**离线开机**（那时 SNTP 还没机会初始化），所以这里每次发现
 * 已联网就调一次 time_sync_start_sntp()（幂等，内部只生效一次）。 */
static void wifi_keepalive_task(void *arg)
{
    (void)arg;
    bool loss_logged = false;

    for (;;) {
        if (net_sta_is_connected()) {
            if (loss_logged) {
                ESP_LOGI(TAG, "network is back");
                loss_logged = false;
            }
            time_sync_start_sntp();
        } else if (net_sta_is_suppressed()) {
            /* 测试模式强制离线：不要在这里把它又连回去 */
            if (!loss_logged) {
                ESP_LOGW(TAG, "OFFLINE (test mode): alarm engine keeps running on "
                              "the local RTC; 'wifi on' to restore");
                loss_logged = true;
            }
        } else {
            if (!loss_logged) {
                ESP_LOGW(TAG, "network lost - alarm engine keeps running on the "
                              "local RTC, will reconnect automatically");
                loss_logged = true;
            }
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* 上电版本检查：只有服务器版本更高才升级。
 * 没配 URL / 检查失败都只是打日志 —— 升级是"顺带的事"，
 * 不能因为它连不上就把温湿度上报和闹钟也拖住。 */
static void boot_version_check(void)
{
    esp_err_t err = ota_check_and_update_on_boot();
    if (err == ESP_ERR_INVALID_STATE) {
        return;                        /* 功能关闭，ota 模块已经打过日志 */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "version check failed (%s) - booting current firmware %s",
                 esp_err_to_name(err), APP_FW_VERSION);
    }
}

/* ------------------------------------------------------------------ */
/* 运行时日志级别：把 MQTT / TLS 两层默认提到 DEBUG。
 *
 * ⚠️ 光调这里是**不够的** —— 本函数只是"运行时的闸门"，而日志能不能
 * 编译进来由 sdkconfig 的 CONFIG_LOG_MAXIMUM_LEVEL 决定（见
 * sdkconfig.defaults 里那段说明）：
 *     esp_log.h 的 ESP_LOGD 带 `#if CONFIG_LOG_MAXIMUM_LEVEL >= 4`，
 * 上限为 INFO(3) 时这些行在**编译期就被整体删掉**，本函数调用后再怎么
 * 设级别也一个字节都打不出来。默认 CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT=y
 * 就会把上限钉在 INFO，所以排查这类问题时必须先在 sdkconfig 里放开到
 * DEBUG 并全量重建，**然后**才轮到本函数生效。
 *
 * 为什么保留这几行而不是直接删掉：esp-mqtt / esp-tls 的 INFO 级只报
 * "出错"，不报"出错前最后收发了什么"。真出问题时（比如 2026-09-14 那次
 * 周期性断连）没有 DEBUG 就只能靠猜 —— 当时正是靠 mqtt_client 的
 * "mqtt_message_receive: total message length" 与 transport 层的
 * f_recv 字节数才定位到 TLS 记录长度超限。删了就再也查不了，所以留着
 * 但默认关。
 *
 * 性能取舍：DEBUG 打开后每条报文会多打十几行，占用 mqtt 任务的栈和时间。
 * 之前注释里写"对 10s 一次上报的负载完全无感"是错的 —— 一上报就来两条
 * 回执（update/accepted ~3 KB + documents ~6 KB），每 10 秒刷上百行串口，
 * 有用的 INFO 全被冲走，还拖慢抓串口的脚本。所以改成默认关闭的开关。 */
#define APP_MQTT_DEBUG_LOGS 0

static void enable_debug_logs(void)
{
#if APP_MQTT_DEBUG_LOGS
    esp_log_level_set("mqtt_client", ESP_LOG_DEBUG);
    esp_log_level_set("transport_base", ESP_LOG_DEBUG);
    esp_log_level_set("esp-tls", ESP_LOG_DEBUG);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_DEBUG);
#endif
}

void app_main(void)
{
    enable_debug_logs();

    ESP_LOGI(TAG, "SHT30 example starting (IDF %s, fw %s)",
             esp_get_idf_version(), APP_FW_VERSION);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip=%s cores=%d", CONFIG_IDF_TARGET, chip.cores);

    ESP_ERROR_CHECK(cfg_init());
    ESP_ERROR_CHECK(net_common_init());

    app_cfg_t cfg;
    cfg_load(&cfg);

    /* 1. 时间：把上电前存下来的 epoch 恢复回来。
     *    没有它，闹钟引擎在离线开机时面对的是 1970 年，只能空转。 */
    time_sync_init();

    /* 2. 传感器 */
    ESP_ERROR_CHECK(sht30_init());

    /* 3. 可控灯（板载 WS2812）：失败不阻塞主流程。
     *    闹钟闪红依赖它，所以失败时要明确告警 —— 闹钟会"��"但看不见。 */
    if (lamp_init() != ESP_OK) {
        ESP_LOGW(TAG, "lamp init failed - alarms will fire but stay invisible");
    }

    /* 3.5 环境传感器（MPU6050 / DHT22 / HC-SR04 / PIR）。
     *
     * **必须在 sht30_init() 和 lamp_init() 之后**，两个依赖：
     *   - SHT30 在 I2C_NUM_1、MPU6050 在 I2C_NUM_0，是两条独立总线，
     *     MPU6050 不借 SHT30 的总线；放在后面是因为本函数会占用
     *     两路 I2C 端口和若干 GPIO，顺序上让基础外设先就绪；
     *   - PIR 会直接开关灯，灯没初始化好就控不了。
     *
     * 失败不阻塞主流程：单路传感器坏掉只影响它自己，温湿度上报和闹钟
     * 照常。采集任务在这里就起来了，但上报要等第 7 步 rest_api_start()
     * 才会真正发出去 —— 期间的事件会被丢弃，这是有意的（开机瞬间的
     * 震动不是有效事件），rest_api_request_report() 在未启动时是空操作。 */
    if (env_sensors_init() != ESP_OK) {
        ESP_LOGW(TAG, "environmental sensors failed to start");
    }

    /* 4. 闹钟引擎：**先于网络**启动，断网/未配网也照常工作 */
    ESP_ERROR_CHECK(alarm_init());
    alarm_set_change_cb(shadow_mark_dirty);

    if (time_sync_start_tasks() != ESP_OK) {
        ESP_LOGW(TAG, "time task failed - epoch will not be persisted");
    }

    /* 5. 配网判断：只有**从来没配过网**才强制进配网 */
    if (cfg.ssid[0] == '\0') {
        run_provisioning_ap("no wifi config");
        return;                       /* 不会走到这里 */
    }

    /* 连不上不再进配网：降级为离线模式继续运行（见文件头的说明） */
    bool online = (net_sta_connect(cfg.ssid, cfg.pass, 20) == ESP_OK);
    if (online) {
        time_sync_start_sntp();
    } else {
        ESP_LOGW(TAG, "WiFi connect failed - entering OFFLINE mode");
        ESP_LOGW(TAG, "  the local alarm engine is already running; WiFi and the "
                      "backend will keep retrying in the background");
        ESP_LOGW(TAG, "  time comes from the recovered NVS epoch until SNTP syncs "
                      "(hold GPIO%d 3s to re-provision)", CONFIG_APP_PTT_GPIO);
    }

    /* 6. 按编译期 APP_BACKEND 启动**唯一**一条上报通道 */
    ESP_LOGI(TAG, "backend = %s (set by APP_BACKEND in main/app_config.h)",
             backend_name(cfg.backend));

    if (cfg.backend == BACKEND_AWS) {
        /* MQTT over TLS 连 AWS IoT Core；连不上会自己重试，不阻塞这里 */
        ESP_ERROR_CHECK(aws_iot_start(&cfg));
#if APP_SHADOW_ENABLE
        /* Device Shadow 闭环：连上后订阅 delta、补报 reported */
        ESP_ERROR_CHECK(shadow_start());
#else
        ESP_LOGW(TAG, "Device Shadow is OFF (APP_SHADOW_ENABLE=0) - "
                      "falling back to the legacy <base>/state + lamp/set topics");
#endif
    } else {
        /* HTTP POST 上报 + 轮询灯命令；REST 不需要任何证书 */
        ESP_ERROR_CHECK(rest_api_start(&cfg));
    }

    xTaskCreate(wifi_keepalive_task, "wifi_keep", 3072, NULL, 4, NULL);
    xTaskCreate(reprovision_btn_task, "reprov_btn", 4096, NULL, 5, NULL);

    /* 串口调试台：设备没有屏幕/按键，闹钟的本地入口靠它（尤其离线测试时）。
     * 起不来也不影响主流程，只打日志。 */
    if (console_start() != ESP_OK) {
        ESP_LOGW(TAG, "serial console failed to start (debug convenience only)");
    }

    /* 这里打印的就是实际上报通道连的地址（全部来自编译期宏） */
    if (cfg.backend == BACKEND_AWS) {
        ESP_LOGI(TAG, "ready. ssid=%s aws=%s:%d thing=%s shadow=%d / "
                      "hold GPIO%d 3s to re-provision.",
                 cfg.ssid, cfg.aws_endpoint, cfg.aws_port, cfg.thing_name,
                 (int)APP_SHADOW_ENABLE, CONFIG_APP_PTT_GPIO);
    } else {
        ESP_LOGI(TAG, "ready. ssid=%s rest=%s://%s:%d%s / hold GPIO%d 3s to re-provision.",
                 cfg.ssid, cfg.rest_use_tls ? "https" : "http", cfg.rest_host,
                 cfg.rest_port, cfg.rest_path, CONFIG_APP_PTT_GPIO);
    }

    /* 7. **最后**才做上电版本检查。
     *
     * 以前它排在启动后端之前，而清单服务器经常不在线，于是每次开机都要
     * 先干等 15s 的 HTTP 超时，MQTT/Shadow 才姗姗来迟 —— 现象是"开机后
     * 十几秒云端看不到设备"，很容易被误判成连接有问题。实测日志里就是
     * ota 在 16.8s 报连接失败、aws_iot 到 16.9s 才开始启动。
     *
     * 移到这里：MQTT 客户端早已在自己的任务里连接（不依赖 app_main），
     * 这个检查只占用 app_main 线程，两者互不干扰。
     * 离线时仍然跳过，没必要等一次注定失败的请求。 */
    if (online) {
        boot_version_check();
    } else {
        ESP_LOGW(TAG, "power-on version check skipped (offline)");
    }

    /* 8. **驻留**：绝不能从 app_main 返回。
     *
     * 主任务返回后 ESP-IDF 会删除它，那块栈（本工程
     * CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584，只有 3.5KB）随即回到堆里，
     * 而 cfg 这个 app_cfg_t 就活在这块栈上。虽然各模块的 start() 都已经
     * 把配置拷进自己的缓冲（详见 rest_api.c 里 s_device 的注释），但
     * **任何**一处漏拷都会变成悬垂指针；更糟的是这类内存错误往往不在
     * 出错点暴露，而是过一会儿才在别处炸开。
     *
     * 2026-09-17 实测的崩溃就是这个形态：app_main 返回（日志里
     * "main_task: Returned from app_main()"）后紧接着
     *   assert failed: tlsf_free tlsf.c:1201
     *   (!block_is_free(block) && "block already marked as free")
     * 回溯落在 prvIdleTask -> prvCheckTasksWaitingTermination -> prvDeleteTCB
     * -> vPortFree，即空闲任务回收**已删除任务**的 TCB 时发现堆已损坏。
     *
     * 所以这里显式挂住。用 portMAX_DELAY 而不是循环 vTaskDelay：不占
     * CPU、不参与调度，纯粹让主任务"存在但不活动"，栈帧全程有效。
     * 想省下这 3.5KB 的话正确做法是**改小**
     * CONFIG_ESP_MAIN_TASK_STACK_SIZE，而不是让 app_main 返回。 */
    ESP_LOGI(TAG, "boot sequence complete - app_main parks (all work is in "
                  "dedicated tasks)");
    vTaskSuspend(NULL);
}
