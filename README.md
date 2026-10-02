# ESP32-S3 Multi-Sensor IoT Node — AWS IoT Core (MQTT + Shadow) & REST Backends

An ESP32-S3 sensor node that samples a **six-axis IMU plus four environmental
sensors**, reports them through **one of two interchangeable backends** — a plain
**REST API** (default) or **AWS IoT Core** (MQTT over mutual TLS with Device
Shadow) — and adds an **offline alarm engine**, SNTP time keeping, **HTTPS OTA**
and a serial console.

```
MPU6050 (I2C0) ─┐
SHT30   (I2C1) ─┤
DHT22   (1-W)  ─┼─▶ env_sensors task ─▶ snapshot ─▶ JSON ─▶ ┌ REST   : POST /report   + GET /lampcmd
HC-SR04 (GPIO) ─┤        (multi-rate)                        └ AWS IoT: MQTT/TLS 8883 + Device Shadow
PIR     (GPIO) ─┘                     events ─▶ event bits ─▶ immediate publish
```

> **Replacing an older document.** The previous Chinese `README.md` described a
> much smaller project ("SHT30 → AWS IoT") and documented configuration fields
> and Kconfig options that no longer exist. It is kept as
> [`README.zh-CN.md`](README.zh-CN.md) for reference, but **this file and the
> code are authoritative**.

---

## Features

| | |
|---|---|
| **Five sensors** | MPU6050 accelerometer/gyroscope, SHT30 temperature/humidity, DHT22 temperature/humidity, HC-SR04 ultrasonic distance, PIR motion |
| **Two backends, one binary shape** | REST (HTTP POST telemetry + GET command polling) or AWS IoT Core (MQTT over TLS with mutual authentication) — selected at compile time by `APP_BACKEND` |
| **Device Shadow closed loop** | `reported` / `desired` / `delta` / `accepted` / `rejected` handling over the standard `$aws/things/<thing>/shadow/*` topics |
| **Offline first** | The alarm engine starts before networking, so alarms still fire with no Wi-Fi; a device that has *ever* been provisioned degrades to offline mode instead of falling back into a captive portal |
| **Cloud-controlled lamp** | On-board WS2812 driven by a cloud command *or* an alarm indicator, arbitrated by a single render task |
| **Vibration and alert detection** | Deviation-based vibration detection (mg threshold, consecutive samples, cooldown) and a bitmask of threshold alerts published with every snapshot |
| **OTA with no-downgrade rule** | HTTPS OTA that compares the remote image version and aborts if it is older |
| **Serial console** | `help` / `time` / `alarm` / `wifi` / `shadow` … for field debugging |
| **Mock server included** | A Flask `rest_mock_server/` reimplements the REST endpoint so the device can be developed without any cloud account |

---

## Hardware

### Bill of materials

| Qty | Item | Bus / address | Notes |
|---|---|---|---|
| 1 | **ESP32-S3-WROOM-1 N16R8** module or dev board | — | 16 MB flash (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`); this firmware does not enable PSRAM |
| 1 | **MPU6050** six-axis IMU breakout (e.g. GY-521) | I²C0, `0x68` (`0x69` optional) | Raw accelerometer + gyroscope + temperature; **no DMP, no attitude fusion**. Accel range configurable, default ±2 g (finest resolution, which is what the vibration detector needs); gyro ±250…2000 dps |
| 1 | **SHT30** temperature / humidity breakout | I²C1, `0x44` | Non-clock-stretching measurement command `0x2400`, CRC-8 checked |
| 1 | **DHT22 / AM2302** temperature / humidity sensor | 1-wire, sampled every 2 s | |
| 1 | **HC-SR04** ultrasonic distance sensor | TRIG + ECHO | 30 ms echo timeout (≈5 m) |
| 1 | **PIR** motion module — HC-SR501 or AM312 | 1 digital input | Presence hold time 30 s; can switch the lamp |
| — | On-board **WS2812** addressable RGB LED | `lamp` output | Green = ON, off = OFF |
| — | On-board **BOOT** button | re-provision input | Hold 3 s to erase Wi-Fi credentials and re-enter provisioning |
| 1 | **1 kΩ + 2 kΩ** resistors | — | Divider on the HC-SR04 `ECHO` line (5 V → 3.3 V) |
| 1 | **4.7 kΩ** resistor | — | DHT22 pull-up — **only for a bare 4-pin sensor**; 3-pin modules already carry one |
| 1 | **5 V supply** (or the board's 5 V/VBUS pin) | — | The HC-SR04 needs 5 V; an HC-SR501 PIR also expects 5 V |

### Wiring / pin map

Defaults compiled from `main/Kconfig.projbuild` (menuconfig → *AWS IoT (SHT30)
Configuration*). Every one of them is configurable there.

| Peripheral | Signal | GPIO | Notes |
|---|---|---|---|
| MPU6050 | SDA | **15** | Its own I²C bus (`I2C_NUM_0`) |
| MPU6050 | SCL | **16** | |
| MPU6050 | AD0 | — | Low → `0x68` (default), high → `0x69` |
| SHT30 | SDA | **12** | `I2C_NUM_1`, 100 kHz |
| SHT30 | SCL | **13** | |
| SHT30 | ADDR | — | Low / floating → `0x44` |
| DHT22 | DATA | **4** | Single-wire; the driver also enables the internal pull-up |
| HC-SR04 | TRIG | **5** | 3.3 V logic is fine |
| HC-SR04 | ECHO | **6** | **5 V output — divide it down** |
| PIR | OUT | **7** | Active high |
| WS2812 lamp | DIN | **48** | On-board LED |
| Re-provision button | — | **0** | On-board BOOT, active low, internal pull-up |
| All sensor modules | VCC | **3V3** | SHT30 / MPU6050 / DHT22 are 3.3 V parts — never power them from 5 V |
| All modules | GND | **GND** | Common ground with the board |

### Wiring notes

* **HC-SR04 `ECHO` is a 5 V output** — use a divider (or a level shifter) before
  the GPIO, otherwise the pin is damaged. `TRIG` is fine at 3.3 V, but the
  sensor itself still wants a **5 V supply**.
* **PIR modules differ**: HC-SR501 needs 5 V but its output is 3.3 V-compatible;
  AM312 runs from 3.3 V. Either way the output goes straight to the GPIO.
* **MPU6050 and SHT30 are on separate I²C buses on purpose** (I²C0 and I²C1):
  they never contend, and the MPU6050 driver owns its own bus object.
* **Never put I²C on GPIO41/42.** On the ESP32-S3 those are the USB D-/D+ lines
  and this project enables the USB Serial/JTAG secondary console; a bus wired
  there works until USB becomes active and then starts NACKing (symptom: both
  `mpu6050` and `SHT30` drop to `--`, plus `i2c.master: unexpected nack` spam).
  GPIO15/16 are free here — use those.
* GPIO0 (BOOT) is the on-board button and is also a strapping pin; the
  re-provision handler only uses it as an input with the internal pull-up.

---

## Software architecture

### Modules (`main/`)

| File | Responsibility |
|---|---|
| `app_main.c` | Startup orchestration: NVS → time → sensors → alarms → provisioning decision → STA → backend; owns the auxiliary tasks |
| `app_config.h` | **The single place for behaviour configuration** (backend, addresses, periods, time zone, shadow switch) — all compile-time macros |
| `app_cfg.c/.h` | Configuration store: NVS holds **only** Wi-Fi credentials; everything else is re-applied from the macros at boot |
| `device_id.c/.h` | Builds the `device` field (`APP_DEVICE_ID`, or `esp32s3_<mac>`) |
| `wifi_net.c/.h` | SoftAP provisioning, STA connection, RSSI, and a **forced-offline switch** for testing |
| `webprov.c/.h` | SoftAP page — **SSID and password only** |
| `sht30.c/.h`, `mpu6050.c/.h`, `dht22.c/.h`, `hcsr04.c/.h`, `pir.c/.h` | Five dependency-free sensor drivers |
| `env_sensors.c/.h` | Application layer: single multi-rate sampling task, alert state machine, vibration detection, PIR→lamp logic |
| `lamp.c/.h` | WS2812 driver with **two-level priority** (logical cloud state vs. alarm indicator) |
| `aws_iot.c/.h` | MQTT over TLS: SNI, ALPN, mutual auth, fragment reassembly |
| `shadow.c/.h` | Device Shadow state machine (reported/desired/delta/accepted/rejected/documents) |
| `rest_api.c/.h` | REST backend: POST snapshots, GET lamp commands |
| `ota_update.c/.h` | Boot-time version check + staged `esp_https_ota` |
| `time_sync.c/.h` | SNTP sync, epoch persisted to NVS, drift observation |
| `alarm.c/.h` | Offline alarm engine (1 Hz, up to 4 alarms) |
| `console.c/.h` | Serial debug console |

### Tasks

| Task | Stack | Prio | Period / trigger |
|---|---|---|---|
| `env_sensors` | 4096 | 4 | MPU 50 ms, PIR 100 ms, HC-SR04 500 ms, DHT22 2 s |
| `msg_proc` (AWS) | 8192 | 5 | MQTT event queue |
| `aws_publish` | 4096 | 5 | publish queue (compatibility mode) |
| `shadow` | 8192 | 4 | shadow document events |
| `rest_api` | 6144 | 5 | report interval / command poll |
| `ota_job` | 16384 | 5 | on boot / on demand |
| `lamp` | 3072 | 4 | render request |
| `time_sync` | 3072 | 3 | SNTP |
| `wifi_keep` | 3072 | 4 | keepalive |
| `reprov_btn` | 4096 | 5 | button edge |
| `console` | 4096 | 2 | UART |

`app_main` suspends itself after bringing everything up rather than returning.

### Data flow

The sampling task publishes a **snapshot** (`env_get_snapshot()`); the backend
serialises it to JSON and sends it. Events — a vibration trigger, an alert bit
flip, a PIR edge — set event bits that wake the reporting task so they go out
immediately instead of waiting for the next period.

Telemetry payload:

```
temperature, humidity, temp_dht, hum_dht, distance_cm, pir, presence,
vibration, accel_dev_mg, alert, rssi, uptime, lamp
```

---

## Configuration

### 1. `main/app_config.h` — behaviour

| Macro | Meaning |
|---|---|
| `APP_BACKEND` | **`0` = REST, `1` = AWS IoT** |
| `APP_REST_HOST` / `_PORT` | REST server, default `192.168.0.101:8080` |
| `APP_REST_USE_TLS` | `0` — plain HTTP by default |
| report interval, time zone, device id | reporting cadence, SNTP TZ, device naming |
| `APP_SHADOW_ENABLE` | enable/disable the Shadow layer |

### 2. `main/aws_certs.h` — endpoint, Thing and certificates

| Macro | Meaning |
|---|---|
| `AWS_IOT_ENDPOINT` | your `xxx-ats.iot.<region>.amazonaws.com` endpoint |
| `AWS_IOT_PORT` | `8883` |
| `AWS_IOT_THING` | Thing name |
| root CA / device certificate / private key | PEM strings compiled into the firmware |

These values are **compiled in, not stored in NVS**, and the firmware refuses to
start the AWS backend while they still contain the `REPLACE_WITH_` placeholder
(`aws_certs_configured()`).

### 3. `menuconfig` (hardware only)

| Option | Default |
|---|---|
| `APP_AP_SSID_PREFIX` / `APP_AP_PASSWORD` | `ESP32S3-SHT30` / empty (open AP) |
| `APP_PTT_GPIO` | 0 |
| `APP_AWS_BASE_TOPIC` | `ESP32S3/esp32s3-sht30` |
| `APP_SHT30_SDA_GPIO` / `_SCL_GPIO` | 12 / 13 |
| `APP_MPU6050_SDA_GPIO` / `_SCL_GPIO` / `_ADDR` / `_ACCEL_RANGE` | 15 / 16 / 0x68 / ±2 g |
| `APP_DHT22_GPIO` | 4 |
| `APP_HCSR04_TRIG_GPIO` / `_ECHO_GPIO` / `_TIMEOUT_US` | 5 / 6 / 30000 |
| `APP_PIR_GPIO` / `_HOLD_MS` / `_LAMP_ENABLE` | 7 / 30000 / y |
| `APP_ENV_SAMPLE_MS` / `_PRINT_MS` | 50 / 1000 |
| vibration: `APP_MPU6050_VIB_THRESH_MG` / `_CONSEC` / `_COOLDOWN_MS` | 300 / 2 / 3000 |
| alerts: temp max/min, humidity max/min, distance min | 35 / 5, 85 / 20, 30 cm |
| `APP_LAMP_GPIO` | 48 |
| `APP_OTA_CHECK_URL` / `_ON_BOOT` / `_TIMEOUT_S` | empty / y / 60 |

### 4. Wi-Fi provisioning

The SoftAP page at `http://192.168.4.1/` asks for **SSID and password only** —
AWS endpoints, certificates and behaviour macros are compile-time values. Saving
writes NVS and reboots. A device that has never been provisioned starts the
SoftAP automatically; hold **GPIO0 for 3 s** to clear credentials and do it
again.

---

## AWS IoT Core setup

1. Create a Thing (name must match `AWS_IOT_THING`) and download its
   certificate, private key and the Amazon Root CA 1.
2. Attach an IoT policy that allows the MQTT topics **and the Shadow topics**:

```
arn:aws:iot:<region>:<account>:topic/<base_topic>/state
arn:aws:iot:<region>:<account>:topic/<base_topic>/lamp/state
arn:aws:iot:<region>:<account>:topic/<base_topic>/lamp/set
arn:aws:iot:<region>:<account>:topic/$aws/things/<thing>/shadow/*
arn:aws:iot:<region>:<account>:topicfilter/$aws/things/<thing>/shadow/*
```

3. Paste the three PEM blocks into `main/aws_certs.h`.
4. `tools/aws_check.py` extracts the PEMs from that header and pre-flights the
   endpoint/certificate before you flash.

The connection uses **SNI + ALPN (`x-amzn-mqtt-ca`) on port 8883**, so a wrongly
provisioned endpoint fails cleanly at the TLS stage.

### Device Shadow reference

| Direction | Key | Note |
|---|---|---|
| `reported` | full sensor snapshot + `lamp` | published on connect and on change |
| `desired` | `lamp`, `report_interval_s` | applied by the device, then echoed into `reported` |
| `delta` | subset of desired | triggers immediate application |
| `accepted` / `rejected` | per-request | version conflicts are handled instead of blindly overwriting |

---

## Backends

**REST (default).** The device is an HTTP *client*: it `POST`s the snapshot to
`http://<host>:8080/report` (2xx = accepted) and `GET`s
`http://<host>:8080/lampcmd` (204 = no command; otherwise `lamp` / `cmd` /
`value` / `state` keys are parsed). It does **not** expose an HTTP server of its
own. `rest_mock_server/` provides a matching Flask implementation, so the whole
device can be tested with no cloud account.

**AWS IoT Core.** MQTT over TLS with mutual authentication; topics are derived
from the base topic:

```
<base>/state        telemetry
<base>/lamp/state   lamp state
<base>/lamp/set     lamp command
```

---

## OTA and versioning

* The firmware version has a single source of truth: `version.cmake`
  (`APP_FW_VERSION`), which is turned into `APP_FW_VERSION_*` macros and
  `PROJECT_VER`.
* `APP_OTA_CHECK_ON_BOOT` (default on) checks a JSON manifest at
  `APP_OTA_CHECK_URL`; the staged OTA API reads the **image description first**
  and aborts if the remote version is older — **downgrades are refused**.
* OTA timeout is 60 s; the image name is `aws_mqtt.bin`.

### Flash layout (`partitions.csv`)

```
nvs         24 KB
otadata      8 KB
phy_init     4 KB
ota_0        4 MB
ota_1        4 MB
storage      1 MB
```

> `partitions.csv` **must stay ASCII-only** — non-ASCII characters in this file
> break the build; the file carries its own warning comment.

---

## Serial console

`help`, `time`, `alarm`, `wifi`, `shadow` and related commands are available on
UART0. The console is deliberately lightweight (its own task at low priority),
so it stays usable while the network is down.

---

## Build & flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM10 flash monitor
```

* Developed with **ESP-IDF v5.5.1**.
* Only one managed component: `espressif/led_strip ^2.4.1`.
* `idf.py erase-flash` before the first flash if you changed the partition
  table.
* Firmware size is roughly 1.1 MB (the older README's "833 KB" is obsolete).

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| AWS backend does not start | `aws_certs.h` still contains `REPLACE_WITH_` placeholders |
| TLS handshake fails altogether | wrong endpoint, or the policy does not allow the Shadow topics |
| Periodic disconnects | TLS record too large — `MBEDTLS_SSL_IN_CONTENT_LEN` is set to 6144 to fit the AWS certificate chain |
| Disconnects under load | keep the MQTT buffers at 8192 (`CONFIG_MQTT_BUFFER_SIZE`); smaller buffers push esp-mqtt into a path that drops messages |
| Shadow `documents` truncated | the 10 KB reassembly buffer exists for the 3–6 KB multi-fragment documents |
| Build fails on `partitions.csv` | the file must be ASCII-only |
| Build fails at configure with `The current CMakeCache.txt directory ... is different than the directory ... where CMakeCache.txt was created` / `The source ... does not match the source ... used to generate cache` | A CMake build tree was copied from another machine (or another ESP-IDF path). `CMakeCache.txt` embeds absolute paths, so it can never be reused: **delete the whole `build/` directory** and rebuild |
| `idf.py fullclean` refuses: *"doesn't seem to be a CMake build directory"* | The same corrupted cache — fullclean will not delete a directory it cannot recognise. Remove `build/` (and any `build_flash/`, `build_win/`) by hand, then rebuild |
| No cloud, but the alarm still rings | by design — the alarm engine is independent of networking |

---

## Engineering notes

Each of these fixes something that was reproducible:

1. **Offline-first startup:** the alarm engine initialises before the network
   stack, so alarms work with no connectivity and survive RTC recovery.
2. **No captive portal for provisioned devices:** only a device that has *never*
   been configured enters SoftAP mode; a provisioned device with no network
   degrades to offline operation instead of hijacking the user's phone.
3. **Shadow document reassembly:** `documents` arrive in 3–6 KB fragments; the
   first version silently dropped them. A 10 KB buffer now reassembles them.
4. **Workaround for esp-mqtt receive disconnects:** setting `.buffer.size` and
   `.out_size` to 8192 keeps payloads out of the fragment path entirely.
5. **TLS record length headroom:** the AWS certificate chain produces a single
   ~5007-byte TLS record; `MBEDTLS_SSL_IN_CONTENT_LEN=6144` stopped the
   resulting periodic disconnects.
6. **Thin MQTT callback:** the event callback only copies and enqueues, so
   PUBACKs are sent on time; parsing happens in a separate task.
7. **Certificate expiry is not validated** (`CONFIG_MBEDTLS_HAVE_TIME_DATE` off)
   so the device can handshake without an RTC; with SNTP now available this can
   be tightened.
8. **OTA uses the staged API** (`begin` → `get_img_desc` → compare → `abort`) to
   reject downgrades before downloading.
9. **Two-level lamp priority:** cloud state and alarm indication converge in one
   render task, which removed the "lamp flashing randomly" behaviour.
10. **Dual I²C buses:** the MPU6050 owns I²C0 and the SHT30 uses I²C1, which
    keeps the two drivers independent (and avoids the GPIO41/42 USB pins).
11. **Simulated offline for acceptance testing:** `net_sta_suppress()` forces the
    offline path on demand.
12. **`app_main` suspends instead of returning**, avoiding a stack-reclaim /
    heap corruption crash.
13. **One multi-rate task** for all sensors: fewer stacks, less scheduling churn,
    and no concurrent I²C access by construction.

---

## Repository layout

```
main/            firmware (see the module table above)
rest_mock_server/  Flask mock backend + README
tools/           aws_check.py pre-flight + helper scripts
partitions.csv   flash layout (ASCII only)
version.cmake    firmware version, single source of truth
README.zh-CN.md  legacy Chinese README (describes an older, smaller version)
```

---

## ⚠️ Certificate handling (read before flashing)

* `main/aws_certs.h` is **tracked by git and ships placeholders only** — the
  endpoint, Thing name and all three PEM blocks read `REPLACE_WITH_...`. The
  firmware refuses to start the AWS backend while that is the case
  (`aws_certs_configured()`), so a fresh clone still **builds** and runs on the
  REST backend out of the box.
* To use AWS: paste your endpoint, Thing name, Amazon Root CA 1, device
  certificate and private key into that file, then rebuild. Keep the real values
  out of git with

  ```bash
  git update-index --skip-worktree main/aws_certs.h
  ```

* A private key that has ever been committed must be treated as compromised:
  **rotate it in AWS IoT Core** (create a new certificate for the Thing, attach
  the same policy, deactivate and delete the old one). Rewriting the file does
  not undo a leak — the key stays in the git history.
* `.gitignore` already excludes `*.pem`, `*.key`, `secret.h`, build directories
  and the local mock-server database, so copying the raw downloaded files into
  the tree is safe from casual commits.

---

## License & contact

Provided as a working reference for ESP32-S3 IoT nodes with AWS IoT Core, Device
Shadow and OTA. Adapt the pin table, partition table and macros to your own
hardware.

**Need ESP-IDF firmware, MQTT/Shadow integration, or an OTA architecture built
for your product?**
→ Reach me on Upwork: `https://www.upwork.com/freelancers/~YOUR_PROFILE_ID`
