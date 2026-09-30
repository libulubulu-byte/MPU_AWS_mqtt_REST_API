/* device_id.c - 设备 ID 组装（详见 device_id.h）
 *
 * 从 rest_api.c 抽出来的，逻辑一字未改 —— 那份是已经在设备上验证过的，
 * 抽公共模块的目的是让 AWS Shadow 分支和 REST 分支**共用同一份**，
 * 避免以后改了一处忘了另一处，出现"REST 上报有 device、Shadow 上报没有"。
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"            /* esp_read_mac / ESP_MAC_WIFI_STA */
#include "sdkconfig.h"

#include "device_id.h"
#include "app_config.h"         /* APP_DEVICE_ID */

static const char *TAG = "device_id";

/* 把 src 拷进 out，只保留 JSON 字符串里安全的字符：
 * 引号 / 反斜杠 / 控制字符一律丢掉。宏正常情况下不会含这些字符，
 * 这里只是防止手滑写错后拼出一个非法 JSON 让服务器 400。 */
static void sanitize(const char *src, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\' || c < 0x20) {
            continue;
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

void device_id_build(char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return;
    }

    if (APP_DEVICE_ID[0]) {
        sanitize(APP_DEVICE_ID, out, out_sz);
        if (out[0]) {
            return;
        }
    }

    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed (%s) - falling back", esp_err_to_name(err));
    }
    snprintf(out, out_sz, "esp32s3_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (err != ESP_OK) {
        /* 读不到 MAC 时上面拼出来是全零，明确标成 unknown 更好排查 */
        snprintf(out, out_sz, "%s", "esp32s3_unknown");
    }
}
