/* env_sensors.h - 环境传感器应用层（MPU6050 / DHT22 / HC-SR04 / PIR）
 *
 * 分层说明：
 *   驱动层（mpu6050.c / dht22.c / hcsr04.c / pir.c）只管"把物理量读出来"，
 *   零业务依赖，可以整段拷到别的工程。
 *   本文件是**应用层**：决定采什么、多久采一次、什么算震动、什么算告警、
 *   灯怎么动。业务规则集中在这里，换项目只需重写这一层。
 *
 * 采集任务：单任务多速率轮询（不为每个传感器开一个任务）。
 *   MPU6050   每 1 个 tick（默认 50ms）—— 震动检测要及时
 *   PIR       每 2 个 tick（100ms）—— 去抖用
 *   HC-SR04   每 10 个 tick（500ms）—— 测距本身要阻塞 30ms 以内
 *   DHT22     每 40 个 tick（2s）—— 器件下限就是 2s
 * 这样做的好处：省栈、省调度开销、天然没有"多个任务同时读 I2C"的问题。
 *
 * I2C 引脚（都在 menuconfig，别照抄这里的默认值，以 sdkconfig 为准）：
 *   SHT30    I2C_NUM_1  GPIO12/13 —— 归 sht30.c 持有
 *   MPU6050  I2C_NUM_0  GPIO15/16 —— 归 mpu6050.c 持有
 * 两者是**两条独立总线**。曾经 MPU6050 借的是 SHT30 那条，实际接线却在
 * 另一对引脚上，于是永远读不到（串口里 "imu --(>=300) vib 0"），排查半天。
 *
 * ⚠️ MPU6050 的引脚**不要**用 GPIO41/42：ESP32-S3 上那是 USB 的 D-/D+，
 * 本工程又开着 USB Serial/JTAG 二级控制台，I2C 挂上去会和 USB 抢脚，
 * 表现为"开机正常、跑一阵开始 NACK"（SHT30 tem -- 加 mpu6050 --）。
 *
 * 串口输出：所有读数由**同一个**周期状态行（每 APP_ENV_PRINT_MS，默认
 * 1000ms）一次性打出，各传感器不再各自打印 —— 每周期一行，便于扫读。
 * 行里带的是"能不能读到"，不是"读到了什么"：
 *   温湿度读不到 -> "--"                      （DHT22 成败）
 *   距离读不到   -> "--(<30)"                  （HC-SR04 成败）
 *   PIR 打两样   -> "absent L" / "present H"   （保持状态 + OUT 原始电平）
 *   MPU6050 读不到 -> "--"                      （I2C 上找不到器件）
 * 所以"全是 --/L/0"就是硬件没接上，而不是"环境恰好正常"。
 *
 * MPU6050 那段打的是**全部读数**，不只一个合加速度：
 *   "a x/y/z g"     三轴加速度，静止平放时 az ≈ 1.00，另两轴 ≈ 0
 *   "g x/y/z dps"   三轴角速度，静止时都 ≈ 0，转动时对应轴明显变化
 *   "t xx.x C"      芯片温度（仅供趋势参考，不是环境温度）
 *   "dev n"         合加速度与 1g 的偏差(mg)，震动判据用的就是它
 * 纵向看同一根轴（比如上一行和这一行的 ax）就能判断模块动没动、
 * 安装方向对不对；"dev" 一直很小说明没触发震动，不是传感器坏了。
 *
 * 震动 / PIR 控灯 / 告警翻转属于**事件**，仍然即时打印，行首带 "EVT"。
 *
 * 事件上报：检测到震动、PIR 状态翻转、告警位变化时调用
 * rest_api_request_report() 请求立即补报一次。该调用是**非阻塞**的
 * （置事件位唤醒上报任务），不会在传感器任务里发起 HTTP 请求。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 告警位（可组合）。上报时用第一个置位的名字，全部为 0 时上报 "none"。 */
#define ENV_ALERT_TEMP_HIGH   (1u << 0)
#define ENV_ALERT_TEMP_LOW    (1u << 1)
#define ENV_ALERT_HUM_HIGH    (1u << 2)
#define ENV_ALERT_HUM_LOW     (1u << 3)
#define ENV_ALERT_DIST_NEAR   (1u << 4)

/* 一次性快照。上报任务调 env_get_snapshot() 取一份，避免在拼 JSON 的
 * 过程中反复加锁、也不会读到半新半旧的数据。 */
typedef struct {
    /* SHT30（I2C，与 DHT22 各测一份，互不影响） */
    bool  sht30_ok;
    float sht30_temp_c;
    float sht30_hum_pct;

    /* DHT22 */
    bool  dht_ok;
    float dht_temp_c;
    float dht_hum_pct;

    /* HC-SR04 */
    bool  dist_ok;
    float distance_cm;

    /* PIR */
    bool  pir_ok;
    bool  presence;         /* 已按 APP_PIR_HOLD_MS 做过保持 */
    bool  pir_raw;          /* 模块 OUT 引脚的瞬时电平（保持之前的原始值） */

    /* MPU6050 —— 六轴原始读数 + 温度，以及派生的偏差/计数。
     * 全部字段在 imu_ok 为 false 时无意义（保留上一次的旧值）。 */
    bool     imu_ok;
    float    imu_ax, imu_ay, imu_az;    /* 加速度，单位 g */
    float    imu_gx, imu_gy, imu_gz;    /* 角速度，单位 °/s (dps) */
    float    imu_temp_c;                /* 芯片温度，单位 °C */
    float    accel_dev_mg;  /* 合加速度与 1g 的偏差，震动判据就是这个 */
    uint32_t vib_count;     /* 累计震动次数 */

    /* 告警位图（ENV_ALERT_*） */
    uint32_t alerts;
} env_snapshot_t;

/* 初始化四路传感器并启动采集任务（幂等）。
 *
 * 前提：lamp_init() 必须**先**调用 —— PIR 要控灯，得先有灯。
 * （MPU6050 自己建 I2C_NUM_0 总线，不再依赖 sht30_init()。sht30_init
 *   是 rest_api 上报温湿度时用的，和本模块的初始化顺序无关。）
 *
 * 任何一路传感器初始化失败都只记日志、置 *_ok=false，**不影响**其它
 * 传感器，也不让本函数返回错误（它们各自独立，一个坏不该全废）。
 *
 * CONFIG_APP_ENV_TASK_ENABLE=n 时直接返回 ESP_OK 且什么都不做。 */
esp_err_t env_sensors_init(void);

/* 通知应用层"云端下发了开/关灯命令"。
 *
 * 当前是空实现：PIR 已改为检测到人时只做绿闪提示（lamp_indicator_start），
 * 不再用 lamp_set() 碰灯的开关状态，所以原先"云端优先"的上锁逻辑已无用
 * 武之地。调用点保留，日后若给 PIR 加回开灯行为，这里是唯一挂载处。
 *
 * 仍与 lamp_set() 在同一次命令处理里成对调用。 */
void env_notify_lamp_command(bool on);

/* 取一份快照（线程安全）。out 不可为 NULL。未初始化时全部置零。 */
void env_get_snapshot(env_snapshot_t *out);

/* 告警位转成上报用的短名字；0 返回 "none"。 */
const char *env_alert_name(uint32_t alerts);

/* 把当前快照打成一行日志（四路读数 + 各自阈值）。
 *
 * 采集任务已按 CONFIG_APP_ENV_PRINT_MS 自动调用它，正常情况下不用手动调；
 * 保留为对外接口是为了 console / 调试命令能随时主动打一份。
 *
 * 这是唯一的周期性传感器输出：各传感器自己的 sample 函数都不打印，
 * 所以串口上每个打印周期只有一行，不会几路日志互相穿插。 */
void env_dump(void);

#ifdef __cplusplus
}
#endif
