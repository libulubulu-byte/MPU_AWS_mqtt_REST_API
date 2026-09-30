/* lamp.c - 板载 WS2812 灯驱动（详见 lamp.h）
 *
 * 单一写入者模型：
 *   lamp_set() / lamp_indicator_start() 只改状态变量，都不直接写像素；
 *   真正写 RMT 的只有渲染任务，50ms 一拍，算出"当前该显示什么颜色"。
 *
 * 好处有三个：
 *   - 两个调用方（云端开关灯、闹钟闪红）不会互相踩像素；
 *   - 颜色没变就不发 RMT，空闲时零开销（开灯后常亮的场景尤其明显）；
 *   - 指示器到期由渲染任务自己发现，不需要谁去定时收尾。
 *
 * 上电/初始化时是"灭"：WS2812 上电本来就有随机残留，必须显式清一次。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"

#include "lamp.h"

static const char *TAG = "lamp";

#define RENDER_PERIOD_MS 50

static led_strip_handle_t s_strip = NULL;
static SemaphoreHandle_t s_lock = NULL;

/* 逻辑状态（云端开关灯） */
static bool s_on = false;

/* 指示器（闹钟闪红），优先级高于逻辑状态 */
static bool     s_ind_active = false;
static uint8_t  s_ind_r = 0, s_ind_g = 0, s_ind_b = 0;
static uint32_t s_ind_on_ms = 200, s_ind_off_ms = 200;
static int64_t  s_ind_start_us = 0;
static int64_t  s_ind_end_us = 0;

/* 上一次真正发出去的像素，用来跳过无变化的刷新 */
static uint8_t s_sent_r = 0, s_sent_g = 0, s_sent_b = 0;
static bool    s_sent_valid = false;

/* 调用者需持有 s_lock */
static void render_locked(void)
{
    if (!s_strip) {
        return;
    }

    uint8_t r = 0, g = 0, b = 0;

    if (s_ind_active) {
        uint32_t period = s_ind_on_ms + s_ind_off_ms;
        uint32_t phase = 0;
        if (period > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - s_ind_start_us) / 1000;
            phase = (uint32_t)elapsed_ms % period;
        }
        if (phase < s_ind_on_ms) {
            r = s_ind_r;
            g = s_ind_g;
            b = s_ind_b;
        }
        /* 灭的半周期保持 0,0,0 —— 这就是"闪" */
    } else if (s_on) {
        /* 标准 WS2812（GRB 字节序），led_strip 组件已按 GRB 发送，
         * 直接传 (0,255,0) 即为绿色，无需对调 */
        g = 255;
    }

    if (s_sent_valid && r == s_sent_r && g == s_sent_g && b == s_sent_b) {
        return;                       /* 无变化 -> 不发 RMT */
    }

    led_strip_set_pixel(s_strip, 0, r, g, b);
    esp_err_t e = led_strip_refresh(s_strip);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "led refresh FAILED: %s", esp_err_to_name(e));
        return;
    }
    s_sent_r = r;
    s_sent_g = g;
    s_sent_b = b;
    s_sent_valid = true;
}

static void render_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            /* 指示器到期：这里收尾，调用方不需要额外定时器 */
            if (s_ind_active && esp_timer_get_time() >= s_ind_end_us) {
                s_ind_active = false;
                ESP_LOGI(TAG, "indicator done -> back to lamp %s",
                         s_on ? "ON" : "OFF");
            }
            render_locked();
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(RENDER_PERIOD_MS));
    }
}

esp_err_t lamp_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_APP_LAMP_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,   /* 10MHz */
        .flags.with_dma = 1,                 /* DMA 发送，避免中断抢占导致时序错乱 */
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led strip init failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    led_strip_clear(s_strip);                /* 上电熄灭 */
    s_on = false;
    s_sent_valid = true;
    s_sent_r = s_sent_g = s_sent_b = 0;

    /* 渲染任务常驻：指示器的到期判定和逻辑状态的颜色恢复都靠它 */
    if (xTaskCreate(render_task, "lamp", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "lamp init ok: GPIO%d (WS2812, off)", CONFIG_APP_LAMP_GPIO);
    return ESP_OK;
}

void lamp_set(bool on)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_on = on;
    /* 指示器生效期间不改像素，但仍更新逻辑状态：
     * 闪烁结束后会自动显示最新的开关状态，而不是闪烁开始时那个旧值。 */
    render_locked();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "lamp %s%s", on ? "ON" : "OFF",
             s_ind_active ? " (indicator running - shown when it ends)" : "");
}

bool lamp_get(void)
{
    return s_on;
}

void lamp_indicator_start(uint8_t r, uint8_t g, uint8_t b,
                          uint32_t on_ms, uint32_t off_ms, uint32_t total_ms)
{
    if (!s_lock) {
        ESP_LOGW(TAG, "indicator ignored: lamp not initialized "
                      "(init failure is non-fatal elsewhere)");
        return;
    }
    if (on_ms == 0) {
        on_ms = 100;                  /* 全灭的半周期没有意义，兜个底 */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* 幂等：已经在闪同样的颜色和节奏，就什么都不做。
     * 没有这道判断，上层只要重复调用（例如每 tick 重算告警），
     * 计时器就被反复归零 —— 表现为日志成对刷屏、灯永远停在第一个
     * 半周期上闪不出节奏。 */
    bool same = s_ind_active &&
                s_ind_r == r && s_ind_g == g && s_ind_b == b &&
                s_ind_on_ms == on_ms && s_ind_off_ms == off_ms;
    if (same) {
        xSemaphoreGive(s_lock);
        return;
    }

    s_ind_r = r;
    s_ind_g = g;
    s_ind_b = b;
    s_ind_on_ms = on_ms;
    s_ind_off_ms = off_ms;
    s_ind_start_us = esp_timer_get_time();
    s_ind_end_us = s_ind_start_us + (int64_t)total_ms * 1000;
    s_ind_active = true;
    render_locked();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "indicator start: rgb(%u,%u,%u) %lu ms on / %lu ms off "
                  "for %lu ms",
             (unsigned)r, (unsigned)g, (unsigned)b,
             (unsigned long)on_ms, (unsigned long)off_ms,
             (unsigned long)total_ms);
}

void lamp_indicator_stop(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ind_active) {
        s_ind_active = false;
        render_locked();
        ESP_LOGI(TAG, "indicator stopped -> lamp %s", s_on ? "ON" : "OFF");
    }
    xSemaphoreGive(s_lock);
}

bool lamp_indicator_active(void)
{
    return s_ind_active;
}
