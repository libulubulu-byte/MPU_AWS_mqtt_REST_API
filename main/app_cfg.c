/* app_cfg.c - 配置存取：NVS 命名空间 "cfg"
 *
 * NVS 里**只剩 WiFi 的 SSID / 密码** —— 这两项是要现场输的，必须能持久化。
 * REST 服务器参数、后端选择、AWS 端点全部来自编译期宏：
 *   main/app_config.h  （REST 地址/端口/路径/后端/轮询周期）
 *   main/aws_certs.h   （AWS 端点 / Thing 名 / 证书）
 * 要改这些值直接改头文件重编译，不再经过配网页。
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "app_cfg.h"

static const char *TAG = "cfg";
static const char *NVS_NS = "cfg";

static const char *K_SSID      = "ssid";
static const char *K_PASS      = "pass";

/* 唯一一份 AWS 默认值：宏写在 aws_certs.h */
#define AWS_DFLT_ENDPOINT AWS_IOT_ENDPOINT
#define AWS_DFLT_PORT     AWS_IOT_PORT
#define AWS_DFLT_THING    AWS_IOT_THING

/* REST 默认值：全部来自 main/app_config.h，改代码即可，不用进 menuconfig */
#define REST_DFLT_HOST  APP_REST_HOST
#define REST_DFLT_PORT  APP_REST_PORT
#define REST_DFLT_PATH  APP_REST_REPORT_PATH
#define REST_DFLT_CMD   APP_REST_CMD_PATH
#define REST_DFLT_TLS   APP_REST_USE_TLS

/* 后端：APP_BACKEND == 1 才是 AWS，其余一律 REST */
#if (APP_BACKEND == 1)
#define BACKEND_DFLT  BACKEND_AWS
#else
#define BACKEND_DFLT  BACKEND_REST
#endif

const char *backend_name(backend_t b)
{
    return (b == BACKEND_AWS) ? "AWS" : "REST";
}

esp_err_t cfg_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* -------- 基础读写 -------- */

static void cfg_get_str(nvs_handle_t h, const char *key, char *dst, size_t len,
                        const char *dflt)
{
    size_t need = len;
    if (nvs_get_str(h, key, dst, &need) != ESP_OK) {
        snprintf(dst, len, "%s", dflt ? dflt : "");
    }
}

void cfg_load(app_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    /* WiFi：唯一从 NVS 读的东西 —— 换路由器了不用重烧固件 */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        cfg_get_str(h, K_SSID, out->ssid, sizeof(out->ssid), "");
        cfg_get_str(h, K_PASS, out->pass, sizeof(out->pass), "");
        nvs_close(h);
    }

    /* 以下全部是编译期常量。每次 cfg_load() 都从宏重新赋值，
     * 而不是"读 NVS 里可能存在的旧值"，这样改了 app_config.h 重烧即生效，
     * 不会被上一版固件写进 NVS 的旧地址盖掉。 */
    snprintf(out->rest_host, sizeof(out->rest_host), "%s", REST_DFLT_HOST);
    snprintf(out->rest_path, sizeof(out->rest_path), "%s", REST_DFLT_PATH);
    snprintf(out->rest_cmd_path, sizeof(out->rest_cmd_path), "%s", REST_DFLT_CMD);
    out->rest_port    = REST_DFLT_PORT;
    out->rest_use_tls = REST_DFLT_TLS;

    snprintf(out->aws_endpoint, sizeof(out->aws_endpoint), "%s", AWS_DFLT_ENDPOINT);
    snprintf(out->thing_name, sizeof(out->thing_name), "%s", AWS_DFLT_THING);
    out->aws_port = AWS_DFLT_PORT;

    out->backend = BACKEND_DFLT;
}

esp_err_t cfg_save(const app_cfg_t *cfg)
{
    /* 只落盘 WiFi。服务器地址之类的编译期项写了也没人读
     * （cfg_load() 一律从宏取），不浪费 flash 擦写寿命。 */
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");

    esp_err_t err = nvs_set_str(h, K_SSID, cfg->ssid);
    if (err == ESP_OK) err = nvs_set_str(h, K_PASS, cfg->pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cfg_erase(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
