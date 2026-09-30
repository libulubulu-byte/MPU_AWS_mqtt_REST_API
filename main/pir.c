/* pir.c - PIR 人体红外传感器驱动（实现）
 *
 * 只依赖 ESP-IDF，不含业务逻辑、不读 Kconfig。详见 pir.h。
 *
 * 为什么不做中断：PIR 的有效信号是"持续几秒到几分钟的高电平"，对毫秒级
 * 响应没有需求。轮询既简单又不会在中断里做耗时操作（中断里只能置标志，
 * 反而多一层复杂度）。上层的采集任务顺带读一下就够了。
 */
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"

#include "pir.h"

static const char *TAG = "pir";

static int s_pin = -1;

esp_err_t pir_init(int gpio_num)
{
    if (gpio_num < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* 没接模块时稳定读 0 = 无人 */
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio %d config", gpio_num);

    s_pin = gpio_num;
    ESP_LOGI(TAG, "init ok: OUT GPIO%d (polled, no ISR)", gpio_num);
    return ESP_OK;
}

void pir_deinit(void)
{
    if (s_pin >= 0) {
        gpio_reset_pin(s_pin);
        s_pin = -1;
    }
}

bool pir_ready(void)
{
    return s_pin >= 0;
}

bool pir_read(void)
{
    if (s_pin < 0) {
        return false;
    }
    return gpio_get_level(s_pin) != 0;
}
