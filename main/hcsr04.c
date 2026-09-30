/* hcsr04.c - HC-SR04 超声波测距驱动（实现）
 *
 * 只依赖 ESP-IDF，不含业务逻辑、不读 Kconfig。详见 hcsr04.h。
 *
 * 时序：TRIG 拉高 10us 触发，模块发出 8 个 40kHz 脉冲，然后把 ECHO
 * 拉高，持续时间 = 声波往返时间。距离 = 高电平时间 × 声速 / 2。
 *
 * 为什么用 esp_timer_get_time() 而不是 RMT/PWM 外设：这个测量只需要
 * "等多高电平"，不要求 RMT 那种精确发波能力。忙等几十毫秒换来的是
 * 代码极简、零外设占用 —— 对 500ms 一次的应用完全划算。
 * 代价是测量期间会占满调用它的任务，所以是任务里轮询、不在中断里做。
 */
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"

#include "hcsr04.h"

static const char *TAG = "hcsr04";

#define TRIG_PULSE_US   10
#define SPEED_OF_SOUND_CM_PER_US  (0.0343f)   /* 343 m/s = 0.0343 cm/us */

/* 一次测量的最小间隔：防止回波还没散尽就发下一次（常见于连续测量场景）。
 * 上层 500ms 一次远大于此，这里只是兜底。 */
#define MIN_INTERVAL_US  60000

static int s_trig = -1;
static int s_echo = -1;
static int64_t s_last_us = 0;

/* 等 echo 变成 level，超时返回 false */
static bool wait_level(int level, uint32_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(s_echo) != level) {
        if ((esp_timer_get_time() - start) > (int64_t)timeout_us) {
            return false;
        }
    }
    return true;
}

esp_err_t hcsr04_init(int trig_gpio, int echo_gpio)
{
    if (trig_gpio < 0 || echo_gpio < 0 || trig_gpio == echo_gpio) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t trig_cfg = {
        .pin_bit_mask = (1ULL << trig_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&trig_cfg), TAG, "trig GPIO%d", trig_gpio);

    gpio_config_t echo_cfg = {
        .pin_bit_mask = (1ULL << echo_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* 没接模块时稳定读 0 */
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&echo_cfg), TAG, "echo GPIO%d", echo_gpio);

    gpio_set_level(trig_gpio, 0);               /* 空闲保持低 */
    s_trig = trig_gpio;
    s_echo = echo_gpio;
    s_last_us = 0;

    ESP_LOGI(TAG, "init ok: TRIG=GPIO%d ECHO=GPIO%d (ECHO must be level-shifted "
                  "to 3.3V)", trig_gpio, echo_gpio);
    return ESP_OK;
}

void hcsr04_deinit(void)
{
    if (s_trig >= 0) {
        gpio_reset_pin(s_trig);
    }
    if (s_echo >= 0) {
        gpio_reset_pin(s_echo);
    }
    s_trig = s_echo = -1;
}

bool hcsr04_ready(void)
{
    return s_trig >= 0 && s_echo >= 0;
}

bool hcsr04_measure_cm(float *cm, uint32_t timeout_us)
{
    if (!hcsr04_ready()) {
        return false;
    }

    int64_t now = esp_timer_get_time();
    if (s_last_us != 0 && (now - s_last_us) < MIN_INTERVAL_US) {
        return false;               /* 离上次太近，直接放弃而不是读回假数据 */
    }
    s_last_us = now;

    /* 1. 触发脉冲：拉高 10us */
    gpio_set_level(s_trig, 1);
    esp_rom_delay_us(TRIG_PULSE_US);
    gpio_set_level(s_trig, 0);

    /* 2. 等回波上升沿。注意要先等它变高 —— 如果模块没接，
     *    内部下拉会一直是 0，这里就会超时返回 false。 */
    if (!wait_level(1, timeout_us)) {
        return false;
    }

    /* 3. 量高电平持续时间 */
    int64_t echo_start = esp_timer_get_time();
    if (!wait_level(0, timeout_us)) {
        return false;               /* 一直高，多半是接线/电平问题 */
    }
    int64_t high_us = esp_timer_get_time() - echo_start;

    /* 4. 换算：往返时间 / 2 × 声速 */
    float dist = ((float)high_us * SPEED_OF_SOUND_CM_PER_US) / 2.0f;

    /* 有效范围保护。HC-SR04 标称 2cm~400cm，超出说明是噪声回波。
     * 这里不再打日志：调用方（env_sensors）每次采样都会把结果打出来，
     * 多打一行只会把串口刷满。 */
    if (dist < 2.0f || dist > 400.0f) {
        return false;
    }

    if (cm) {
        *cm = dist;
    }
    return true;
}
