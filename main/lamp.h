/* lamp.h - 板载 WS2812 灯（GPIO48，默认开=绿、关=灭）
 *
 * 两级优先级：
 *   1. **指示器（indicator）** —— 闹钟闪红等临时性的闪烁，优先级最高；
 *   2. 逻辑状态（lamp_set / lamp_get）—— 云端开关灯，指示器结束后自动恢复。
 *
 * 为什么要有优先级：只有一颗灯，但有两个写入者（云端开关灯 + 闹钟闪红）。
 * 谁调用谁直接写像素的话，闪烁会被开关灯打断、开关灯也会被闪烁覆盖，
 * 现象是"灯自己乱闪"。现在所有像素写入都收敛到一个渲染任务，
 * 按优先级取色 —— 逻辑状态在指示器期间只被记录、不被显示，
 * 指示器一结束立刻按逻辑状态恢复。lamp_get() 始终返回逻辑状态，
 * 因此上报到 Shadow 的 lamp 字段不会被闪烁污染。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化灯并默认熄灭（幂等）。 */
esp_err_t lamp_init(void);

/* 开灯 / 关灯。 */
void lamp_set(bool on);

/* 查询当前逻辑状态（不受闪烁影响）。 */
bool lamp_get(void);

/* 让灯按 (r,g,b) 闪烁 total_ms：亮 on_ms、灭 off_ms 交替。
 * 期间覆盖逻辑状态；total_ms 到期后自动回落到 lamp_set() 的颜色。
 * 重复调用会重置计时并换成新颜色。 */
void lamp_indicator_start(uint8_t r, uint8_t g, uint8_t b,
                          uint32_t on_ms, uint32_t off_ms, uint32_t total_ms);

/* 立刻结束指示器，恢复逻辑状态。 */
void lamp_indicator_stop(void);

/* 指示器是否正在生效。 */
bool lamp_indicator_active(void);

#ifdef __cplusplus
}
#endif
