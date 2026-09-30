/* wifi_net.c - WiFi 管理
 *
 * AP 阶段：创建默认 AP netif + 启动热点，供网页配网。
 * STA 阶段：创建默认 STA netif，事件回调维护“已联网(Got IP)”标志。
 *
 * 重连策略：
 *   - 等 WIFI_EVENT_STA_START（驱动就绪）后才发起第一次 esp_wifi_connect()，
 *     避免 start 后立刻 connect 被驱动丢弃；
 *   - 连接等待期内，每次 WIFI_EVENT_STA_DISCONNECTED 都立刻重试，
 *     直到 xEventGroupWaitBits 超时。路由器握手偶发失败很常见，
 *     原来"只试一次"会导致首次开机就误判成连不上而掉进配网模式。
 *   - 连上之后由上层 wifi_keepalive_task 每 5s 补一次 esp_wifi_connect()。
 */
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "wifi_net.h"

static const char *TAG = "wifi";

#define EVT_CONNECTED BIT0

static EventGroupHandle_t s_evt = NULL;
static volatile bool s_sta_connecting = false;   /* 正在等待连接（需要自动重试）*/
static volatile bool s_suppressed = false;       /* 测试用强制离线：不重连 */

/* 把原因码翻译成人话，方便排错 */
static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:            return "auth expire";
    case WIFI_REASON_AUTH_FAIL:              return "AUTH_FAIL (wrong password?)";
    case WIFI_REASON_ASSOC_FAIL:             return "assoc fail";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "4-way handshake timeout (wrong password / weak signal)";
    case WIFI_REASON_CONNECTION_FAIL:        return "connection fail";
    case WIFI_REASON_NO_AP_FOUND:            return "NO_AP_FOUND (wrong SSID / 5GHz only / out of range)";
    case WIFI_REASON_BEACON_TIMEOUT:         return "beacon timeout (AP lost)";
    case WIFI_REASON_AP_TSF_RESET:           return "AP restarted";
    case WIFI_REASON_ROAMING:                return "roaming";
    default:                                 return "see wifi_err_reason_t";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 驱动就绪，这里才是发起连接的正确时机 */
        if (s_sta_connecting) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 注意：data 是 wifi_event_sta_disconnected_t*，不能强转成 int* */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = d ? d->reason : 0;
        ESP_LOGW(TAG, "STA disconnected, reason=%u (%s)", reason, reason_str(reason));
        xEventGroupClearBits(s_evt, EVT_CONNECTED);
        if (s_suppressed) {
            ESP_LOGW(TAG, "auto-reconnect suppressed (offline test mode)");
        } else if (s_sta_connecting) {
            ESP_LOGI(TAG, "retrying...");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_evt, EVT_CONNECTED);
    }
}

esp_err_t net_common_init(void)
{
    if (s_evt != NULL) {
        return ESP_OK;   /* 已初始化 */
    }

    s_evt = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_evt, ESP_ERR_NO_MEM, TAG, "event group");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, NULL),
        TAG, "reg wifi evt");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, NULL),
        TAG, "reg ip evt");
    return ESP_OK;
}

esp_err_t net_ap_start(const char *ssid, const char *pass)
{
    esp_netif_create_default_wifi_ap();

    /* 可能刚从 STA 失败切换过来：先停掉 WiFi 再以 AP 模式重启，确保生效 */
    s_sta_connecting = false;
    esp_wifi_stop();

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid) - 1);
    if (pass && strlen(pass) > 0) {
        strncpy((char *)ap.ap.password, pass, sizeof(ap.ap.password) - 1);
        ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set ap mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "set ap cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");

    ESP_LOGI(TAG, "AP started: \"%s\" -> http://192.168.4.1", ssid);
    return ESP_OK;
}

esp_err_t net_sta_connect(const char *ssid, const char *pass, int timeout_s)
{
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    /* 密码为空 -> 按开放网络连接；有密码 -> 要求 WPA/WPA2 */
    if (pass && strlen(pass) > 0) {
        sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set sta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "set sta cfg");

    xEventGroupClearBits(s_evt, EVT_CONNECTED);
    s_sta_connecting = true;      /* 事件回调负责发起连接并在断开后重试 */

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");
    ESP_LOGI(TAG, "connecting to \"%s\" (password %s, max %ds)...",
             ssid, (pass && pass[0]) ? "set" : "EMPTY -> open network", timeout_s);

    EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_s * 1000));
    s_sta_connecting = false;

    if ((bits & EVT_CONNECTED) == 0) {
        ESP_LOGW(TAG, "connect timeout after %ds", timeout_s);
        return ESP_ERR_TIMEOUT;
    }

    /* 长期通电的传感器节点：modem sleep 省电，链路不断 */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "set ps MIN_MODEM failed: %s", esp_err_to_name(ps));
    }
    return ESP_OK;
}

bool net_sta_is_connected(void)
{
    if (s_evt == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_evt) & EVT_CONNECTED) != 0;
}

int net_sta_rssi(void)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        return (int)info.rssi;
    }
    return 0;
}

void net_sta_suppress(bool suppress)
{
    if (suppress == s_suppressed) {
        return;
    }
    s_suppressed = suppress;

    if (suppress) {
        /* 先关掉"自动重试"这个开关，否则 esp_wifi_disconnect() 触发的
         * DISCONNECTED 事件会立刻把连接又拉起来，等于没断。 */
        s_sta_connecting = false;
        esp_wifi_disconnect();
        ESP_LOGW(TAG, "offline test mode ON: WiFi disabled until 'wifi on'");
    } else {
        s_sta_connecting = true;
        ESP_LOGW(TAG, "offline test mode OFF: reconnecting");
        esp_wifi_connect();
    }
}

bool net_sta_is_suppressed(void)
{
    return s_suppressed;
}
