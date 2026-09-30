/* device_id.h - 设备 ID（上报 JSON 里 "device" 字段的值）
 *
 * 两个后端（REST / AWS Shadow）都要在报文体里带设备标识，逻辑完全一样，
 * 所以抽到这里共用一份实现 —— 以前 rest_api.c 里有一份 static 副本，
 * 再加一份就必然漂移。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 组装设备 ID 写进 out（含结尾 '\0'）：
 *   APP_DEVICE_ID 非空 -> 用它，但先过滤掉会被坏 JSON 的字符；
 *   为空            -> 用芯片出厂 MAC 拼 "esp32s3_aabbccddeeff"。
 * 读 MAC 失败时兜底 "esp32s3_unknown"，**绝不返回空串** ——
 * 服务器侧靠这个字段区分设备，空串会让多台设备混成一条记录。
 * out 建议至少 48 字节。 */
void device_id_build(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
