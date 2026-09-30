/* dht22.c - DHT22 / AM2302 温湿度传感器驱动（实现）
 *
 * 只依赖 ESP-IDF，不含业务逻辑、不读 Kconfig。详见 dht22.h。
 *
 * 单总线时序（数据手册）：
 *   主机拉低 >1ms  -> 释放 -> 从机应答 80us 低 + 80us 高
 *   之后每个 bit：50us 低 + (26-28us 高 = 0 / 70us 高 = 1)
 *   最后 40 bit = 5 字节：湿度高/低、温度高/低、校验和
 *
 * 因为要按微秒采样，读取期间必须屏蔽中断，否则 WiFi 中断一插进来
 * 采到的电平就全错了（表现为校验和不匹配）。屏蔽窗口约 5ms，
 * 对 WiFi/BLE 是可接受的短暂延迟。
 *
 * 时间基准用 esp_rom_sys.h 的 esp_rom_delay_us()：它是忙等，
 * 精度在微秒级，且不像 vTaskDelay 那样受 tick 粒度影响。
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "dht22.h"

static const char *TAG = "dht22";

#define DHT22_MIN_INTERVAL_US   (2 * 1000 * 1000)   /* 器件要求 ≥2s */
#define DHT22_START_LOW_US      1200                /* 主机起始拉低 >1ms */
#define DHT22_RESPONSE_US       100                 /* 等待从机应答的超时 */
#define DHT22_BIT_TIMEOUT_US    100                 /* 单个 bit 的超时 */

/* 文件级状态 */
static int     s_pin = -1;
static int64_t s_last_read_us = 0;
static int     s_fail_in_a_row = 0;

/* 等电平变高/变低，超时返回 false。忙等，微秒级。 */
static bool wait_level(int pin, int level, uint32_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return false;
        }
    }
    return true;
}

/* 读一个 bit：先等上升沿，再量高电平持续多久。
 * 高电平 26-28us = 0，70us = 1。用 40us 做分界最稳。 */
static bool read_bit(int pin, int *bit, uint32_t timeout_us)
{
    if (!wait_level(pin, 1, timeout_us)) {
        return false;                       /* 没等到上升沿 */
    }
    int64_t t0 = esp_timer_get_time();
    if (!wait_level(pin, 0, timeout_us)) {
        return false;                       /* 一直是高，超时 */
    }
    int64_t high_us = esp_timer_get_time() - t0;
    *bit = (high_us > 40) ? 1 : 0;
    return true;
}

static bool dht22_read_raw(uint8_t out[5])
{
    int pin = s_pin;
    if (pin < 0) {
        return false;
    }

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bool ok = false;

    /* --- 起始信号 --- */
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(DHT22_START_LOW_US);
    gpio_set_level(pin, 1);                 /* 开漏，拉高即释放，靠外部上拉 */
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    /* 到这里为止还没屏蔽中断：起始低电平是毫秒级的，被中断打断也没关系 */
    portENTER_CRITICAL(&mux);

    /* --- 从机应答：80us 低 + 80us 高 --- */
    if (!wait_level(pin, 0, DHT22_RESPONSE_US)) {
        goto done;
    }
    if (!wait_level(pin, 1, DHT22_RESPONSE_US)) {
        goto done;
    }
    if (!wait_level(pin, 0, DHT22_RESPONSE_US)) {
        goto done;
    }

    /* --- 40 bit 数据，高位在前 --- */
    memset(out, 0, 5);
    for (int i = 0; i < 40; i++) {
        int bit = 0;
        if (!read_bit(pin, &bit, DHT22_BIT_TIMEOUT_US)) {
            goto done;
        }
        if (bit) {
            out[i / 8] |= (uint8_t)(0x80 >> (i % 8));
        }
    }
    ok = true;

done:
    portEXIT_CRITICAL(&mux);
    return ok;
}

/* ------------------------------------------------------------------ */

esp_err_t dht22_init(int gpio_num)
{
    if (gpio_num < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,       /* 模块自带 4.7k，这里算双保险 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio %d config", gpio_num);

    s_pin = gpio_num;
    s_last_read_us = 0;
    s_fail_in_a_row = 0;

    ESP_LOGI(TAG, "init ok: data GPIO%d (min interval 2s, module pull-up assumed)",
             gpio_num);
    return ESP_OK;
}

void dht22_deinit(void)
{
    if (s_pin >= 0) {
        gpio_reset_pin(s_pin);
        s_pin = -1;
    }
}

bool dht22_ready(void)
{
    return s_pin >= 0;
}

bool dht22_read(float *temp_c, float *hum_pct)
{
    if (s_pin < 0) {
        return false;
    }

    /* 节流：器件要求 2s 间隔，太快直接拒绝而不是读回垃圾数据。
     * 日志限频，避免上层 50ms 轮询时刷屏。 */
    int64_t now = esp_timer_get_time();
    if (s_last_read_us != 0 && (now - s_last_read_us) < DHT22_MIN_INTERVAL_US) {
        if (s_fail_in_a_row == 0) {
            ESP_LOGD(TAG, "read too soon (<2s), skipped");
        }
        s_fail_in_a_row++;
        return false;
    }
    s_last_read_us = now;

    uint8_t d[5];
    if (!dht22_read_raw(d)) {
        if (++s_fail_in_a_row == 1 || (s_fail_in_a_row % 10) == 0) {
            ESP_LOGW(TAG, "no response (%d in a row)", s_fail_in_a_row);
        }
        return false;
    }

    /* 校验和：前 4 字节之和的低 8 位 */
    uint8_t sum = (uint8_t)(d[0] + d[1] + d[2] + d[3]);
    if (sum != d[4]) {
        if (++s_fail_in_a_row == 1 || (s_fail_in_a_row % 10) == 0) {
            ESP_LOGW(TAG, "checksum mismatch (%d in a row): %02X%02X%02X%02X != %02X",
                     s_fail_in_a_row, d[0], d[1], d[2], d[3], d[4]);
        }
        return false;
    }

    /* 湿度 16 位 = 整数部分(8) + 小数部分(8)，单位 %RH（DHT22 有小数）
     * 温度 16 位：最高位为符号位，1 表示零下 */
    uint16_t raw_hum = (uint16_t)((d[0] << 8) | d[1]);
    uint16_t raw_temp = (uint16_t)((d[2] << 8) | d[3]);

    float hum = raw_hum / 10.0f;
    float temp;
    if (raw_temp & 0x8000) {
        temp = -(float)(raw_temp & 0x7FFF) / 10.0f;
    } else {
        temp = (float)raw_temp / 10.0f;
    }

    /* 数据手册标称范围，越界说明数据不可信（常见于供电不足） */
    if (hum < 0.0f || hum > 100.0f || temp < -40.0f || temp > 80.0f) {
        ESP_LOGW(TAG, "out of range: %.1f%%RH %.1fC - discarded", hum, temp);
        s_fail_in_a_row++;
        return false;
    }

    if (s_fail_in_a_row > 0) {
        ESP_LOGI(TAG, "recovered after %d failure(s)", s_fail_in_a_row);
        s_fail_in_a_row = 0;
    }
    if (temp_c) {
        *temp_c = temp;
    }
    if (hum_pct) {
        *hum_pct = hum;
    }
    return true;
}
