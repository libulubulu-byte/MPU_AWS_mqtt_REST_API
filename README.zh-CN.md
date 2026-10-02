# AWS_mqtt —— SHT30 温湿度 → MQTT over TLS → AWS IoT Core

```
SHT30 传感器
   ↓ (I2C, 0x44)
ESP32-S3 读取数据                    板载 WS2812 灯
   ↓ (WiFi + MQTT over TLS 8883)        ↑ (订阅 lamp/set 下发)
AWS IoT Core (设备证书双向认证)          |
   ↓ (规则引擎 / Lambda / 控制台)        |
下游存储或面板 ──────── 点开关 ─────────┘
```

- **证书写在 `main/aws_certs.h` 的宏里**，编译期烧进固件（不经过配网热点的明文 HTTP）；
- WiFi 账号、AWS 端点、Thing 名用 **SoftAP 网页配网**填进 NVS，换路由器 / 换端点不用重新编译；
- 走 **MQTT over TLS 8883，双向认证**（设备验 broker、broker 验设备），并按要求
  协商 ALPN `x-amzn-mqtt-ca`；
- 温湿度是**上报**（设备 → 云端），灯开关是**订阅**（云端 → 设备），同一个连接双向走；
- **没有** Home Assistant 自动发现那一套：AWS IoT Core 只是消息通道，
  展示交给 IoT 控制台的 MQTT test client、规则引擎或你自己的后端。

> 分工原则：**会变的**（WiFi / 端点 / Thing 名）走网页配网；**要保密的**（私钥）
> 和**不常变的**（证书）编译进固件。换证书要改头文件重新烧写。

## 1. 硬件接线

| SHT30 | ESP32-S3 | 说明 |
|---|---|---|
| VCC | 3V3 | 3.3V，不要接 5V |
| GND | GND | |
| SDA | GPIO12 | 默认值，可在 menuconfig 改 |
| SCL | GPIO13 | 默认值，可在 menuconfig 改 |

I2C 端口 `I2C_NUM_1`，100 kHz，从机地址 `0x44`（ADDR 引脚悬空/接地）。
（`I2C_NUM_0` 留给 MPU6050，见 menuconfig 的 MPU6050 项，默认 SDA=GPIO15 / SCL=GPIO16。
**不要**把 MPU6050 接到 GPIO41/42：ESP32-S3 上那是 USB 的 D-/D+，本工程开着 USB
Serial/JTAG 二级控制台，I2C 挂上去会和 USB 抢同一对脚，症状是"刚开机正常、跑一阵
开始报 `i2c.master: unexpected nack`"，状态行里 SHT30 和 mpu6050 一起变 `--`。）
BOOT 键（GPIO0）用作配网按键：**长按 3 秒清空配置并重启进入配网**。

## 2. 在 AWS 侧准备证书与策略

1. **创建 Thing**：*IoT Core → Manage → All devices → Things → Create things*
   → 选 *Create a single thing*，名字例如 `esp32s3-sht30`
   （这个名字后面要填进配网页的 *Thing Name*，它同时是 MQTT Client ID）；
2. **下载证书**：*Auto-generate a new certificate*，然后一次性下载三个文件
   （页面只给一次机会下载，丢了就重新创建证书）：
   - 设备证书 `xxxxxxxx-certificate.pem.crt` → 填进 `AWS_IOT_CERT_PEM`
   - 私钥 `xxxxxxxx-private.pem.key` → 填进 `AWS_IOT_KEY_PEM`
   - 根 CA：*Download* 链接给的是 Amazon Root CA，也可自己从
     <https://www.amazontrust.com/repository/> 取 `AmazonRootCA1.pem`
     → 填进 `AWS_IOT_CA_PEM`
     （**注意别把 `.pem.crt` 设备证书当成根 CA**）；
   然后按下一节把三个文件的内容填进 `main/aws_certs.h`；
3. **创建策略**（*IoT Core → Security → Policies → Create policy*）并绑定到该证书。
   最小可用策略（把 `<region>` / `<account>` / `esp32s3-sht30` 换成你的值）：

   ```json
   {
     "Version": "2012-10-17",
     "Statement": [
       {
         "Effect": "Allow",
         "Action": "iot:Connect",
         "Resource": "arn:aws:iot:<region>:<account>:client/esp32s3-sht30"
       },
       {
         "Effect": "Allow",
         "Action": ["iot:Publish", "iot:Receive"],
         "Resource": "arn:aws:iot:<region>:<account>:topic/esp32s3-sht30/*"
       },
       {
         "Effect": "Allow",
         "Action": "iot:Subscribe",
         "Resource": "arn:aws:iot:<region>:<account>:topicfilter/esp32s3-sht30/*"
       }
     ]
   }
   ```

   > 状态主题 `<base>/state` 走 Publish；`<base>/lamp/set` 走 Subscribe + Receive。
   > 少了 `Receive` 能订阅但收不到消息，少了 `Subscribe` 连订阅都会被拒。
4. **记下端点**：*IoT Core → Settings → Device data endpoint*，形如
   `xxxxxxxxxxxx-ats.iot.ap-northeast-1.amazonaws.com`（**不带** `https://`、**不带**端口）。

## 3. 填入证书（`main/aws_certs.h`）

打开 `main/aws_certs.h`，把三个宏里的占位符替换成第 2 步下载的文件内容。
每个宏用相邻字符串字面量拼接，**每行末尾必须有 `\n`**：

```c
#define AWS_IOT_CA_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n" \
    "...（中间每行同样以 \n 结尾）...\n" \
    "-----END CERTIFICATE-----\n"
```

手工粘贴 20 多行 base64 很容易漏掉 `\n`，建议用脚本生成（WSL / Linux）：

```bash
cd examples/get-started/AWS_mqtt
{
  echo '#pragma once'
  echo '#include <string.h>'
  echo '#include <stdbool.h>'
  for pair in "AWS_IOT_CA_PEM:AmazonRootCA1.pem" \
              "AWS_IOT_CERT_PEM:xxxxxxxx-certificate.pem.crt" \
              "AWS_IOT_KEY_PEM:xxxxxxxx-private.pem.key"; do
    macro=${pair%%:*}; file=${pair#*:}
    echo "#define $macro \\"
    sed 's/\\/\\\\/g; s/"/\\"/g; s/$/\\n" \\/' "$file" | sed 's/^/    "/'
    echo '    ""'
    echo
  done
  sed -n '/aws_certs_configured/,$p' main/aws_certs.h   # 保留末尾的检查函数
} > /tmp/aws_certs.h && mv /tmp/aws_certs.h main/aws_certs.h
```

填好后 `aws_certs_configured()` 会返回 true（它检查占位符 `REPLACE_WITH_`
是否还在）。如果没填就烧进去，启动时会直接报
`certificates not filled in - edit main/aws_certs.h`，而不是反复 TLS 握手失败。

> `aws_certs.h` 含私钥，**别提交到公开仓库**。建议加入 `.gitignore`
> 或改用 `aws_certs.h.example` + 本地生成。

## 4. 编译与烧写

目标板 **ESP32-S3-WROOM-1 N16R8**（16MB flash / 8MB PSRAM），分区表见 `partitions.csv`：

| 分区 | 大小 | 用途 |
|---|---|---|
| `nvs` | 24 KB | WiFi / AWS 端点配置（配网页写在这） |
| `otadata` | 8 KB | OTA 启动槽标志 |
| `phy_init` | 4 KB | PHY 校准数据 |
| `ota_0` / `ota_1` | 各 4 MB | 双应用槽（目前只用 ota_0，以后加 OTA 直接可用） |
| `storage` | 1 MB | 预留（SPIFFS） |

当前固件约 833 KB（证书按 4KB 估算），**app 槽剩余 80%**。

```bash
. $IDF_PATH/export.sh          # Windows: . $env:IDF_PATH\export.ps1
cd examples/get-started/AWS_mqtt
idf.py set-target esp32s3
idf.py build flash monitor     # 可选 -p COMx
```

> 换分区表后直接 `idf.py flash` 即可：bootloader / 分区表 / otadata / app 都会写到各自正确偏移，
> NVS 不动，**配网信息不会丢**。
> 只有启动异常时才需要 `idf.py erase-flash flash` 全擦重来（那会清掉 NVS，要重新配网）。

## 5. 首次配网（SoftAP 网页）

1. 上电后设备没配置过，会自动开热点，串口打印：
   `connect to AP "ESP32S3-SHT30-A1B2C3D4E5F6" (pass "") and open http://192.168.4.1`
2. 手机/电脑连上该热点（默认**开放无密码**，可用 `APP_AP_PASSWORD` 设密码）；
3. 浏览器打开 **http://192.168.4.1**（部分手机需先关掉移动数据的“自动切换”）；
4. 页面只有一组字段，填完点 *Save & Reboot*：

   | 字段 | 说明 |
   |---|---|
   | WiFi SSID / Password | 路由器 2.4G 账号密码，开放网络密码留空 |
   | AWS IoT Endpoint | 第 2 步记下的端点域名，**不要**带 `https://` 或端口 |
   | Port | 默认 `8883`（MQTT over TLS）。本固件不跑 WebSocket，别填 443 |
   | Thing Name | 与 AWS 上的 Thing 名一致，同时用作 MQTT Client ID |

   WiFi 密码框**留空表示保持原值**，其它字段以页面预填值为准。
   页面上**没有**证书相关的东西 —— 它们在固件里（第 3 节）。

重新配网：**长按 BOOT 键 3 秒** → 清空配置 → 重启回配网模式。
（证书在固件里，配网不会动它们。）

## 6. 配置项（`idf.py menuconfig` → *AWS IoT (SHT30) Configuration*）

配网页面上能填的都有 Kconfig 默认值（页面预填的就是这些），另外这些只能在编译期定：

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `APP_AP_SSID_PREFIX` | `ESP32S3-SHT30` | 配网热点名前缀，实际带 MAC 尾号 |
| `APP_AP_PASSWORD` | 空 | 热点密码，空 = 开放；≥8 字符为 WPA2 |
| `APP_PTT_GPIO` | `0` | 配网按键（低有效），0 = BOOT 键 |
| `APP_DEFAULT_AWS_ENDPOINT` | 空 | 配网页预填的端点 |
| `APP_DEFAULT_AWS_PORT` | `8883` | 配网页预填端口 |
| `APP_DEFAULT_THING_NAME` | `esp32s3-sht30` | 配网页预填的 Thing 名 |
| `APP_AWS_BASE_TOPIC` | `esp32s3-sht30` | 主题前缀，需与 IoT 策略里的 Resource 匹配 |
| `APP_PUBLISH_INTERVAL_S` | `10` | 上报周期（秒）。AWS 按消息计费，调试时再改 1 |
| `APP_SHT30_SDA_GPIO` / `APP_SHT30_SCL_GPIO` | `12` / `13` | I2C 引脚 |
| `APP_LAMP_GPIO` | `48` | 板载 WS2812 灯的 GPIO（开=绿，关=灭） |

## 7. MQTT 主题

主题前缀来自 `CONFIG_APP_AWS_BASE_TOPIC`，下面用 `<base>` 代指
（Kconfig 默认是 `esp32s3-sht30`，本仓库 `sdkconfig` 里是 `ESP32S3/esp32s3-sht30`，
以实际构建的 `sdkconfig` 为准，别照抄本文档）。

**用哪一组主题取决于 `APP_SHADOW_ENABLE`**（`main/app_config.h`，编译期开关）。
两组是**互斥**的：不是"都开着"，而是只有一个会编进固件。

### 7a. Shadow 模式（`APP_SHADOW_ENABLE=1`，当前配置）

| topic | 方向 | payload |
|---|---|---|
| `$aws/things/<thing>/shadow/update` | 设备发 `reported`；云端发 `desired` | 见 `main/shadow.c` 头部注释 |
| `$aws/things/<thing>/shadow/update/delta` | AWS → 设备 | **只含** desired 与 reported 有差异的字段 |
| `$aws/things/<thing>/shadow/update/accepted` | AWS → 设备 | 影子文档 + metadata + version |
| `$aws/things/<thing>/shadow/update/rejected` | AWS → 设备 | 错误响应（典型是 version 冲突 409） |
| `$aws/things/<thing>/shadow/update/documents` | AWS → 设备 | previous/current 完整文档（实测 6 KB 级，接收缓冲要放得下） |
| `$aws/things/<thing>/shadow/get` | 设备 publish（空载荷） | 请求当前文档 |

⚠️ **开了 Shadow 之后，裸主题那一组不会被发布。** `<base>/state`、
`<base>/lamp/state`、`<base>/lamp/set` 的整套实现都在
`#if APP_SHADOW_ENABLE == 0` 里（见 `aws_iot.c`），编译期就被删掉了。
所以任何订阅 `<base>/#` 的 IoT 规则、后台或 test client **永远收不到数据**，
必须改成订阅上面的 Shadow 主题。这是排查"规则/后台没数据"时最先该看的一条。

### 7b. 兼容模式：裸主题（`APP_SHADOW_ENABLE=0`）

| topic | 方向 | payload |
|---|---|---|
| `<base>/state` | 上报 | `{"temperature":25.3,"humidity":60.1,"rssi":-58,"uptime":123}` |
| `<base>/lamp/state` | 上报 | `ON` / `OFF` |
| `<base>/lamp/set` | **订阅**（云端 → 设备） | `ON` / `OFF` / `1` / `0` |

- 上线后先订阅 `lamp/set`（QoS 1），再立刻发一次温湿度和灯状态；
- 这一组**不用 retain**：AWS IoT Core 的保留消息要额外开
  *Retain messages* 才有，默认不支持；后端的“当前值”请自己在数据库里存
  （裸主题没有状态概念，设备离线时下发的命令直接丢）；
- 不能和 Shadow 同时开：两条通道都能改灯会导致状态打架。

- 断开重连由 esp-mqtt 自动做（5s 起，指数退避）。

## 8. 云端验证与下发

**看设备数据**：*IoT Core → MQTT test client → Subscribe to a topic*。

- **Shadow 模式**（当前）填 `$aws/things/<thing>/shadow/#`，每 10 秒一条
- 裸主题模式填 `<base>/#`
- ⚠️ Shadow 模式下**不要**订 `<base>/#` 等数据 —— 设备不往那儿发，
  订阅会一直静默，很容易被误判成"设备没上报"

**从云端下发指令**（Shadow 模式）：同一个 test client 的 *Publish to a topic*，
主题 `$aws/things/<thing>/shadow/update`：

```json
{"state":{"desired":{"lamp":"ON"}}}              // 开灯（板载 LED 变绿）
{"state":{"desired":{"lamp":"OFF"}}}             // 关灯
{"state":{"desired":{"report_interval_s":30}}}   // 改上报周期（5~3600，免重烧）
{"state":{"desired":{"alarm_set":{"id":1,"enable":true,"hour":7,"minute":30,
                                  "days":[1,2,3,4,5],"duration_s":10}}}}
```

支持的 desired 键见 `main/shadow.c` 头部注释。注意两点：

- 被**拒绝**的键不会从 desired 里清除（云端能看出"这条件没生效"），
  所以测完拒绝路径要手工清：`{"state":{"desired":{"lamp":null}}}`
- 想让设备重新接收同一条指令，也要先清掉 desired —— 否则 desired 与
  reported 无差异，AWS 不会再发 delta

**从云端开灯**（裸主题模式）：*Publish to a topic* → 主题 `<base>/lamp/set`，
载荷 `ON`（或 `OFF`），设备板载 LED 立刻变色，并回一条 `<base>/lamp/state`。

**用 AWS CLI 看/发**（需配好凭证与区域）：

```bash
aws iot-data publish --topic 'esp32s3-sht30/lamp/set' --payload 'ON' \
  --cli-binary-format raw-in-base64-out

# 订阅需要用到 mqtt 子命令（AWS CLI v2 的 iot 不直接提供订阅）
```

## 9. 代码结构

| 文件 | 职责 |
|---|---|
| `main/app_main.c` | 启动流程：NVS → 配网判断 → STA 连接 → SHT30 → AWS MQTT；配网按键任务 |
| `main/aws_certs.h` | **三份证书的编译期宏**（`AWS_IOT_CA_PEM` / `_CERT_PEM` / `_KEY_PEM`），自己填 |
| `main/app_cfg.c/.h` | 配置 NVS 读写：WiFi + AWS 端点/端口/Thing 名（不含证书） |
| `main/wifi_net.c/.h` | WiFi 管理：SoftAP 配网热点 / STA 连接 / RSSI |
| `main/webprov.c/.h` | SoftAP 配网网页：`GET /` 配置页、`POST /save` 保存 WiFi+端点并重启 |
| `main/sht30.c/.h` | SHT30 驱动（I2C 新驱动、单次测量 + CRC 校验） |
| `main/lamp.c/.h` | 板载 WS2812 灯驱动（led_strip / RMT） |
| `main/aws_iot.c/.h` | MQTT over TLS 连 AWS IoT Core、SNI/ALPN、双向认证、周期上报、灯命令订阅 |
| `main/Kconfig.projbuild` | 编译期配置项 |
| `main/idf_component.yml` | 组件依赖（led_strip） |

## 10. 故障排查

| 现象 | 排查方向 |
|---|---|
| 串口报 `certificates not filled in - edit main/aws_certs.h` | `aws_certs.h` 里还有 `REPLACE_WITH_` 占位符，三份 PEM 没填全 |
| 串口 `transport error: tls=0x... ` 反复重连 | 证书/密钥不配对（用错了 Thing 的 key）；把设备证书当成了根 CA；PEM 行尾漏了 `\n`；端点写成了带 `https://` 的 URL |
| TLS 报证书过期/尚未生效 | 设备没有 RTC，开机时间默认 1970，而 AWS 证书校验依赖时间。需要先同步时间（SNTP），或在 `sdkconfig` 里开 `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`（**仅调试**） |
| 连上就被踢、日志反复 `connected` / `disconnected` | 两台设备用了同一个 Client ID（Thing 名）。每台设备必须有独立 Thing 与证书 |
| 连上了但收不到 lamp/set | IoT 策略少了 `iot:Subscribe` 或 `iot:Receive`，或主题 Resource 没匹配到 `<base>/*` |
| 能被订阅但发不出 state | 策略少了 `iot:Publish`，或被 *Basic Ingest* / 主题上限限制 |
| 一上电就进配网模式 | 没配过网，或 STA 连接 20s 超时（SSID/密码错、路由器是 5G、信号弱） |
| 打不开 192.168.4.1 | 手机连着热点但走了移动数据；关掉“WLAN 助理/自动切换网络” |
| 配网页保存后还是进配网 | WiFi 密码框留空且之前没存过密码；重新填一次 |
| 改完 sdkconfig 烧不进去 / 名字对不上 | 工程名已从 `ha_mqtt` 改成 `aws_mqtt`，产物是 `aws_mqtt.bin`；旧的 `build/` 目录请删掉重新 `idf.py build` |
| 烧写报 `Address "xxx.bin" must be a number` | **`partitions.csv` 里不能有非 ASCII 字符（中文注释）**。构建用 `parttool.py` 查分区偏移，它按系统默认编码打开 CSV（中文 Windows 是 GBK），UTF-8 中文会解码失败 → app 偏移查不到 → flash_args 里 app 那行只剩文件名。改成英文注释即可；`main/idf_component.yml` 同理保持纯 ASCII |
| 串口只有 `SHT30 read failed` | 接线/上拉；确认 SDA=GPIO12、SCL=GPIO13，地址 0x44 |
| 想换 WiFi / 端点 / Thing 名 | 长按 BOOT 键 3 秒重新配网 |
| 想换证书 | 改 `main/aws_certs.h` 后重新 `idf.py flash`（配网不动证书） |
# MPU_AWS_mqtt_REST_API
# MPU_AWS_mqtt_REST_API
