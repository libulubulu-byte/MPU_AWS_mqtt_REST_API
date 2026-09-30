/* app_config.h - 设备行为配置的**唯一**修改入口
 *
 * 配网页（SoftAP）只负责填 WiFi 名称和密码，其它一律固定在这里。
 * 需要改服务器地址、端口、路径、后端类型、版本号等，改本文件的宏后
 * 重新编译烧录即可，不用联网进配网页。
 *
 * 和另外两个配置文件的职责划分：
 *   app_config.h   <- 本文件：服务器地址/端口/路径/后端/轮询周期等
 *   aws_certs.h    <- AWS IoT 端点、Thing 名、三份 PEM 证书
 *   Kconfig        <- 只在 menuconfig 里调的量（GPIO、上报间隔、OTA URL）
 *
 * 为什么不用 Kconfig：改宏比进 menuconfig 快，而且这些值是"我自己的
 * 服务器"，不属于需要随产物变化的构建选项。GPIO/间隔这类和硬件绑定的
 * 量仍留在 Kconfig，方便 menuconfig 里统一看。
 */
#pragma once

/* ==================================================================== */
/* 1. 上报通道：REST API 还是 AWS IoT Core                               */
/* ==================================================================== */
/* 0 = REST API（HTTP，不需要证书）
 * 1 = AWS IoT Core（MQTT over TLS 8883，需要 aws_certs.h 里的证书）
 *
 * 注意：两个模块会各自占用到服务器的 TCP/TLS 连接，运行时只跑一个。
 *
 * 切到 AWS(1) 时必须先做的一件事：AWS IoT 策略里要放行 Device Shadow 主题
 *   arn:aws:iot:<region>:<account>:topic/$aws/things/<Thing>/shadow/...
 *   arn:aws:iot:<region>:<account>:topicfilter/$aws/things/<Thing>/shadow/...
 * 现有策略只放行了 "<Thing>/..." 这一条，而 Shadow 主题是
 * $aws/things/<Thing>/shadow/...，**不以 Thing 名开头**，那条通配匹配不到。
 * 漏配的表现极具误导性：MQTT 连得上、日志不报错，就是 delta 收不到、
 * reported 石沉大海（被策略静默拒绝，没有任何回执）。 */
#define APP_BACKEND             0

/* ==================================================================== */
/* 1b. 设备 ID                                                          */
/* ==================================================================== */
/* 上报 JSON 里的 "device" 字段，服务器靠它区分是哪台设备。
 * 它和 AWS 的 Thing 名是两回事，所以单独一个宏，不借 AWS_IOT_THING：
 *   - 走 REST 时也要有值（AWS_IOT_THING 只在 AWS 分支有意义）
 *   - 留空("") = 自动用芯片 MAC 拼成 "esp32s3_aabbccddeeff"，同一份固件
 *     烧到多台设备即各自唯一，不用逐台改这一行
 *   - 填了值则原样使用，便于人工指定可读名
 * 只允许 ASCII 字母数字、下划线、连字符 —— 字段直接拼进 JSON，
 * 含引号/反斜杠会破坏报文体（rest_api.c 里会再过滤一次，双保险）。 */
#define APP_DEVICE_ID           ""

/* ==================================================================== */
/* 2. REST 服务器参数                                                    */
/* ==================================================================== */
/* 服务器地址：域名或 IP，**不要**带 "http://"，不要带端口。
 * 设备发出的 host 必须是它自己能访问到的地址：
 *   - 不要用 localhost / 127.0.0.1（那是设备自己）
 *   - 用你跑 mock 后台那台电脑的局域网 IP，例如 192.168.0.101
 *   - 改了 IP 记得同步改这里并重新烧录 */
#define APP_REST_HOST           "192.168.0.101"

/* 服务器端口。本工程附带的 rest_mock_server 默认 8080 */
#define APP_REST_PORT           8080

/* 状态上报路径（POST）。payload 形如：
 * {"device":"...","version":"1.0.0","temperature":25.3,"humidity":60.1,
 *  "rssi":-58,"uptime":123,"lamp":"ON"}
 * 服务器返回任意 2xx 即视为成功，body 内容不解析。 */
#define APP_REST_REPORT_PATH    "/report"

/* 灯命令轮询路径（GET）。响应须是一个小 JSON，按顺序识别这些键：
 *   {"lamp":"ON"} / {"lamp":"OFF"} / {"cmd":...} / {"value":...} / {"state":...}
 * 没有命令时回 204、空 body 或 {"lamp":"none"} 都可以。
 * 设备只在状态变化时才动作，并立刻补报一次新状态。 */
#define APP_REST_CMD_PATH       "/lampcmd"

/* 是否用 HTTPS。0 = 明文 HTTP（局域网调试推荐）；
 * 1 = HTTPS，此时要么填下面的 CA，要么忽略证书（自签证书会握手失败）。*/
#define APP_REST_USE_TLS        0

/* 走 HTTPS 时的服务器 CA（PEM，用 \n 转义写成一行）。
 * 留空 = 用固件内置根证书包校验 —— 自签证书的测试服务器必然失败，
 * 这时要么把自签 CA 填进来，要么干脆把 APP_REST_USE_TLS 关掉。 */
#define APP_REST_CA_PEM         ""

/* 灯命令轮询间隔（毫秒）。1000 足够让 LED 跟手，又不会把服务器刷爆。 */
#define APP_REST_POLL_MS        1000

/* 状态上报间隔（秒）。同时用于 AWS MQTT 分支的上报周期。 */
#define APP_REPORT_INTERVAL_S   10

/* ==================================================================== */
/* 3. 固件版本号（"MAJOR.MINOR.PATCH"）                                  */
/* ==================================================================== */
/* 用途有三处：开机日志、REST 上报的 version 字段、OTA 版本比较。
 * 上电时拿它和 OTA 清单里的版本比，**只有服务器版本更高才升级**，
 * 等于或更低都保持当前固件（不降级）。
 *
 * ⚠️ 不要在这里改版本号！本工程的版本号唯一来源是工程根目录的
 *    version.cmake，main/CMakeLists.txt 会把它转成 -D 传进来，
 *    从而同时保证「编译宏」和「镜像头 App version」一致。
 *
 *    下面这三个 #ifndef 只是兜底：万一这个头文件被非 CMake 的构建
 *    系统单独编译，也不会因为宏未定义而报错。正常构建时它们都会被
 *    CMake 传入的值覆盖掉。
 *
 *    发新固件：改 version.cmake 里的一行，重新编译即可。 */
#ifndef APP_FW_VERSION_MAJOR
#define APP_FW_VERSION_MAJOR    0
#endif
#ifndef APP_FW_VERSION_MINOR
#define APP_FW_VERSION_MINOR    0
#endif
#ifndef APP_FW_VERSION_PATCH
#define APP_FW_VERSION_PATCH    0
#endif

/* ==================================================================== */
/* 4. Device Shadow（仅 AWS 后端有效）                                   */
/* ==================================================================== */
/* 1 = 用 Device Shadow 做主通道（desired/delta -> 应用 -> reported 回读），
 * 0 = 退回老的裸主题 <base>/state 与 <base>/lamp/set。
 *
 * ⚠️ 这是**编译期**开关：aws_iot.c 用 #if 把整个兼容通道包起来（订阅
 * lamp/set、周期发 state 的那个任务都在里面）。改成 0 必须重新编译烧写，
 * 配网页和 NVS 都改不了它。
 *
 * 两条通道没有"都开着"的选项：同时能改灯会导致状态打架 —— 灯被谁改的、
 * 该以哪个为准，排查时会非常痛苦。只有一个真相来源，所以互斥。
 *
 * （曾有一个 APP_LEGACY_LAMP_SET 宏，想表达"Shadow 开着时单独开关
 *   lamp/set"，但真正的编译条件一直是本宏，那个宏定义了却从未被任何代码
 *   引用 —— 改它不会有任何效果。已删除，免得后来者白改。） */
#define APP_SHADOW_ENABLE       1

/* reported 周期上报间隔（秒）。Shadow 会保存状态，但温湿度是时效数据，
 * 不报上去云端就一直是旧值，所以仍需周期上报。
 * 运行期可被 desired 里的 report_interval_s 覆盖（5~3600）。 */
#define APP_SHADOW_REPORT_S     10

/* ==================================================================== */
/* 5. 时间与离线闹钟                                                     */
/* ==================================================================== */
/* 时区。闹钟按**本地时间**语义，报文体里的 epoch 一律 UTC。
 * 格式是 POSIX TZ：CST-8 表示东八区（符号与直觉相反，UTC+8 要写 CST-8）。 */
#define APP_TZ                  "CST-8"

/* epoch 小于这个值视为"时间不可信"（对应 2020-09-13）。
 * 上电时 RTC 归零到 1970，如果不设这道闸，时间就变成 1970，
 * 闹钟引擎会以为今天是 1970 年的某天而乱算。 */
#define APP_TIME_MIN_VALID      1600000000L

/* 把当前时间写回 NVS 的最小间隔（秒）。写得太勤会磨 NVS 擦写寿命
 * （24KB 分区扛得住，但没必要）：600s = 每天 144 次。
 * 掉电后时间误差上限就是这个值加上 RTC 自身漂移。 */
#define APP_TIME_SAVE_S         600

/* 打印 "TIME ..." 一行的间隔（秒）。这行是**离线漂移测量的原始数据**：
 * 抓串口脚本会给每行加 PC 墙钟前缀，两者相减就是设备 RTC 的实时误差。 */
#define APP_TIME_LOG_S          60

/* SNTP 服务器（最多 3 个，见 sdkconfig 的 CONFIG_LWIP_SNTP_MAX_SERVERS）。
 * 国内优先 aliyun，兜底 pool.ntp.org。 */
#define APP_SNTP_SERVER_1       "ntp.aliyun.com"
#define APP_SNTP_SERVER_2       "cn.pool.ntp.org"
#define APP_SNTP_SERVER_3       "pool.ntp.org"

/* 闹钟条数上限。注意与 alarm.c 里 NVS blob 的结构体布局绑定：
 * 改这个值等于改了落盘格式，老数据会被版本号挡住并当作"无闹钟"重新开始。 */
#define APP_ALARM_MAX           4

/* 闹钟响铃默认时长（秒），可被 desired 里的 duration_s 覆盖 */
#define APP_ALARM_DURATION_S    10

/* 过期容忍窗口（秒）：闹钟点已过但没超过这个时长时补响（设备刚从离线/
 * 重启恢复的典型场景）；超过则只标记 missed 不补响 —— 否则半夜开机
 * 会把当天所有闹钟连环放一遍。 */
#define APP_ALARM_GRACE_S       60
