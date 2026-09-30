/* ota_update.c - 上电版本比较 + esp_https_ota 升级
 *
 * 为什么用 esp_https_ota_begin/get_img_desc/perform 分步 API，而不是
 * 一句 esp_https_ota()：
 *   - 分步可以在**下载第一个字节之前**先读出新镜像头里的版本号
 *     （esp_https_ota_get_img_desc），再和本机版本比；
 *   - 于是"低版本不处理"这条规则能在真正写 flash 之前就拦住，
 *     而不是先擦掉一个 OTA 槽再发现不该升。
 *
 * 顺序上很关键：先 begin() -> get_img_desc() 比较 -> 不高就 abort()，
 * 高才继续 perform() 下载。
 *
 * 是否升级以**镜像头里的版本**为准（以清单 JSON 为提示），这样即使
 * 服务器 JSON 写错了版本号，也不会出现"清单说 1.0.5、实际推的是 1.0.0"
 * 这种把设备刷低的情况。
 *
 * 不碰回滚（CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 默认关闭）：新镜像
 * 被 esp_https_ota_finish() 标记为有效后直接启动，失败了下次上电会再
 * 检查一遍，行为简单可预期。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"

#include "ota_update.h"
#include "app_version.h"

static const char *TAG = "ota";

#define OTA_HTTP_TIMEOUT_MS  (CONFIG_APP_OTA_TIMEOUT_S * 1000)
#define MANIFEST_MAX_LEN     512

/* ------------------------------------------------------------------ */
/* 版本清单                                                            */
/* ------------------------------------------------------------------ */

/* 从 JSON 里取字符串键值：支持 "key":"value" 与 '"key": "value"'，
 * 没必要为此引入 cJSON —— 清单是我们自己发的，格式固定。 */
static bool manifest_get(const char *json, const char *key,
                         char *out, size_t out_sz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_sz) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

/* 在栈上分配：这个函数只在启动路径调用一次，且返回前就退出作用域。
 * （放 static 反而会让 512 字节常驻 DRAM。） */
static esp_err_t fetch_manifest(char *out, size_t out_sz)
{
    esp_http_client_config_t hc = {
        .url = CONFIG_APP_OTA_CHECK_URL,
        .timeout_ms = 15000,
        /* 版本清单是公开信息，不含密钥，用内置根证书包即可；
         * 服务器用自签证书时把 bundle 换掉改成 .cert_pem */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&hc);
    ESP_RETURN_ON_FALSE(c, ESP_FAIL, TAG, "manifest client init");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET %s failed: %s", CONFIG_APP_OTA_CHECK_URL,
                 esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }

    (void)esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "manifest -> HTTP %d", status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    int total = 0;
    while (total < (int)out_sz - 1) {
        int n = esp_http_client_read(c, out + total, out_sz - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    out[total] = '\0';

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (total == 0) {
        ESP_LOGE(TAG, "manifest is empty");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 下载并写入被动分区                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t perform_ota(const char *url, const char *want_version,
                            const char *running_version)
{
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,       /* 4MB 镜像下载，复用连接省握手 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &hc,
    };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 镜像头里的版本号 —— 这里才是"要不要刷"的最终依据 */
    esp_app_desc_t img;
    err = esp_https_ota_get_img_desc(h, &img);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot read image header: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        return err;
    }

    ESP_LOGI(TAG, "image: version=%s project=%s (running %s)",
             img.version, img.project_name, running_version);

    if (!app_version_is_newer(img.version, running_version)) {
        ESP_LOGW(TAG, "image version %s is not newer than %s - aborting "
                      "(downgrade/equal is not applied)",
                 img.version, running_version);
        esp_https_ota_abort(h);
        return ESP_OK;                   /* 不算错误：低版本本来就不处理 */
    }
    if (want_version[0]) {
        /* 清单版本和镜像头版本对上才正常。用两个方向判断而不是
         * app_version_is_newer()，因为它对 "相等" 返回假 —— 拿它单独
         * 判断会把"两边一致"这种最健康的场景误报成告警。
         *   img > want : 镜像比清单还新，多半是清单忘了更新，值得记一笔
         *   img < want : 清单在撒谎 / 发错文件，同样是配置问题
         *   img == want: 正常，不必吭声 */
        if (app_version_is_newer(img.version, want_version)) {
            ESP_LOGW(TAG, "image header says %s but manifest only announced %s "
                          "- manifest is stale", img.version, want_version);
        } else if (app_version_is_newer(want_version, img.version)) {
            ESP_LOGW(TAG, "manifest announced %s but image header says %s "
                          "- serving the wrong file?", want_version, img.version);
        } else {
            ESP_LOGI(TAG, "version check ok: manifest %s == image %s",
                     want_version, img.version);
        }
    }

    int image_size = esp_https_ota_get_image_size(h);
    ESP_LOGI(TAG, "downloading image (%d bytes)...", image_size);

    int last_pct = -1;
    while (true) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int read = esp_https_ota_get_image_len_read(h);
        if (image_size > 0) {
            int pct = (int)((int64_t)read * 100 / image_size);
            if (pct / 10 != last_pct / 10) {   /* 每 10% 打一行，别刷屏 */
                last_pct = pct;
                ESP_LOGI(TAG, "ota %d%% (%d/%d)", pct, read, image_size);
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        return err;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "incomplete download - not applying");
        esp_https_ota_abort(h);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "OTA to %s applied - rebooting", img.version);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;                       /* 不会走到这里 */
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

/* OTA 参数：入口函数把参数放到这里，专用任务读取。
 * 只在启动路径用一次，不存在并发。 */
typedef struct {
    const char *url;
    const char *want_version;
    const char *running_version;
    esp_err_t   result;
} ota_job_t;

/* ⚠️ 必须是**静态**存储，不能是 perform_ota_in_task() 的栈上局部变量：
 * 早先这里是 `ota_job_t job;` 然后把 &job 传给 xTaskCreate()，而 ota_task
 * 在结束后调用 vTaskDelete(NULL) —— 任务自己先没了，它用过的 job 却仍被
 * 等待方读（job.result）。栈帧一旦先被回收，读到的就是垃圾；配合
 * app_main 返回（主任务被删）更会变成对已释放内存的访问。
 * 实测崩溃形态是空闲任务回收 TCB 时命中
 *   tlsf_free tlsf.c:1201 (!block_is_free(block) && "block already marked as free")
 * 即堆已被写坏。改静态后 job 的生命周期与任何任务栈都无关。 */
static ota_job_t s_ota_job;

typedef enum {
    OTA_JOB_IDLE = 0,
    OTA_JOB_RUNNING,
} ota_job_state_t;
static volatile ota_job_state_t s_ota_job_state = OTA_JOB_IDLE;

/* 为什么 OTA 必须在**独立任务**里跑，而不是直接在 app_main 里调：
 *
 * esp_https_ota_finish() 内部会调 esp_ota_end()，后者要对写进 flash 的
 * 整个镜像做 SHA-256 校验。这一路会拉起 mbedtls 的 sha256 上下文和
 * esp_ota 的内部缓冲，栈需求远超 app_main 的栈。
 *
 * ⚠️ 别按"默认值"估这个栈：本工程实测 sdkconfig 里
 * CONFIG_ESP_MAIN_TASK_STACK_SIZE=**3584**（只有 3.5KB，不是早先注释
 * 里写的 8KB —— 那句是错的，照它估算会严重低估溢出风险）。
 * 而 app_main 这条链上 ota_check_and_update_on_boot() 还带着一个
 * char manifest[512]，本身就很紧张。实测会在 100% 下载完成后
 * 命中 vApplicationStackOverflowHook 复位，otadata 还没写，
 * 于是下次开机又看到"服务器更新"→ 无限循环升级。
 *
 * 所以这里起一个 16KB 栈的专用任务来跑真正的工作，app_main 那边
 * 只是阻塞等待结果——库的栈开销由这个任务承担，跟 app_main 解耦。 */
#define OTA_TASK_STACK   16384

static void ota_task(void *arg)
{
    (void)arg;
    s_ota_job.result = perform_ota(s_ota_job.url, s_ota_job.want_version,
                                   s_ota_job.running_version);
    /* 正常升级会在这里之前 esp_restart()，走不到下面。
     * 只是把状态标记为"结束了"，让等待方跳出轮询。 */
    s_ota_job_state = OTA_JOB_IDLE;
    vTaskDelete(NULL);
}

/* 在专用任务里执行 OTA，阻塞直到任务结束（正常升级会直接重启，不返回）。
 *
 * ⚠️ 这里**不用信号量**，而是轮询 s_ota_job_state，原因是消除一个真实的
 * 竞态：早先用 xSemaphoreGive(job->done) 通知、等待方醒来立刻
 * vSemaphoreDelete(job->done) —— 但 give 只是"标记可获取并让等待者就绪"，
 * 此刻 ota_task 很可能**还没真正退出**（后面还有 vTaskDelete(NULL)），
 * 于是"删信号量"和"任务还在用栈/TCB"重叠，堆被写坏；症状正是空闲任务
 * 回收 TCB 时命中
 *   tlsf_free: block already marked as free
 * 然后 panic 重启（2026-09-17 实测）。
 *
 * 轮询 20ms 一次：OTA 是"顺带的事"，且绝大多数路径会直接重启，这点延迟
 * 完全可以忽略，换来的是确定性的生命周期。 */
static esp_err_t perform_ota_in_task(const char *url, const char *want_version,
                                     const char *running_version)
{
    s_ota_job.url             = url;
    s_ota_job.want_version    = want_version;
    s_ota_job.running_version = running_version;
    s_ota_job.result          = ESP_FAIL;
    /* ⚠️ 必须在 xTaskCreate() **之前**置 RUNNING：否则新任务可能在
     * 创建返回后、我们还没赋值前就跑完并置回 IDLE，随后的 while 会
     * 永远等不到状态变化而死循环（OTA 一失败就卡死整个启动流程）。 */
    s_ota_job_state = OTA_JOB_RUNNING;

    if (xTaskCreate(ota_task, "ota_job", OTA_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "cannot spawn ota task - running in current stack "
                      "(may overflow, consider raising CONFIG_ESP_MAIN_TASK_STACK_SIZE)");
        s_ota_job_state = OTA_JOB_IDLE;
        return perform_ota(url, want_version, running_version);
    }

    while (s_ota_job_state != OTA_JOB_IDLE) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_ota_job.result;
}

esp_err_t ota_check_and_update_on_boot(void)
{
    const char *running = APP_FW_VERSION;

    if (!CONFIG_APP_OTA_CHECK_ON_BOOT) {
        ESP_LOGI(TAG, "power-on OTA check disabled (APP_OTA_CHECK_ON_BOOT=n)");
        return ESP_ERR_INVALID_STATE;
    }
    /* CONFIG_APP_OTA_CHECK_URL 是字符串宏，可以为 ""（表示功能未配置），
     * 所以这里必须用 strlen 而不是 CONFIG_APP_OTA_CHECK_URL[0] —— 后者在
     * 空串时会展开成 ""[0]，等价于解引用一个空指针。 */
    if (strlen(CONFIG_APP_OTA_CHECK_URL) == 0) {
        ESP_LOGI(TAG, "no OTA URL configured - skip version check "
                      "(running %s)", running);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "power-on version check: running %s, url=%s",
             running, CONFIG_APP_OTA_CHECK_URL);

    char manifest[MANIFEST_MAX_LEN];
    esp_err_t err = fetch_manifest(manifest, sizeof(manifest));
    if (err != ESP_OK) {
        return err;                      /* 连不上不影响正常业务 */
    }

    char version[32] = { 0 };
    char url[192] = { 0 };
    if (!manifest_get(manifest, "version", version, sizeof(version))) {
        ESP_LOGE(TAG, "manifest has no \"version\": %s", manifest);
        return ESP_FAIL;
    }
    if (!manifest_get(manifest, "url", url, sizeof(url))) {
        ESP_LOGE(TAG, "manifest has no \"url\": %s", manifest);
        return ESP_FAIL;
    }

    int cmp = app_version_cmp(version, running);
    ESP_LOGI(TAG, "manifest version=%s -> %s (local %s)", version,
             cmp > 0 ? "NEWER" : (cmp == 0 ? "SAME" : "OLDER"), running);

    if (cmp <= 0) {
        ESP_LOGI(TAG, "%s - nothing to do", cmp == 0 ? "already up to date"
                                                     : "server is older");
        return ESP_OK;                   /* 相等/更低：不处理 */
    }

    ESP_LOGW(TAG, "upgrading %s -> %s from %s", running, version, url);
    return perform_ota_in_task(url, version, running);
}
