# REST 测试后台（配合固件 REST 后端）

给 `AWS_mqtt_REST_API` 工程的 REST 分支提供服务器端：接收设备上报、下发灯命令、
并在浏览器里显示实时数据、历史曲线和灯开关。

不需要 Docker，不需要云服务，Windows 上双击 bat 就能跑。

## 一键启停

| 脚本 | 作用 |
| --- | --- |
| `start.bat` | 启动服务，自动查本机 IP、自动缺啥装啥、自动开浏览器 |
| `stop.bat` | 停止服务（先按 PID 杀，再按端口兜底扫） |
| `status.bat` | 看进程/端口/HTTP 是否正常 |
| `test_api.bat` | 模拟设备跑一遍全部接口，验证服务端协议是否正确 |

双击 `start.bat` 即可。首次运行会自动 `pip install flask`。

服务在后台独立运行，关掉 bat 窗口不影响；要停就双击 `stop.bat`。

## 设备侧要填的配置

`start.bat` 会直接打印出来，对照填进设备配网页（SoftAP 热点 → 打开配网页面）：

| 配网页字段 | 值 | 说明 |
| --- | --- | --- |
| `backend` | REST API | 默认就是这个 |
| `rest_host` | 如 `192.168.0.101` | 电脑的局域网 IP，**不能填 localhost** |
| `rest_port` | `8080` | |
| `rest_path` | `/report` | 上报接口 |
| `rest_cmd` | `/lampcmd` | 灯命令轮询接口 |
| `rest_tls` | 关 | 明文 HTTP，避免自签证书的坑 |

改端口的话同时改 `server.py` 的 `PORT`/环境变量和设备配网页，两边要一致。
路径可以用环境变量覆盖：`REPORT_PATH` / `CMD_PATH`。

## 硬件接线与灯

灯的命令是**轮询式**的：设备每 1 秒（`APP_REST_POLL_INTERVAL_MS`）GET 一次
`/lampcmd`。你在控制台点「开灯」，命令会排队，设备下一轮取走后立即清除，
取走后设备会马上补报一次状态，所以 UI 上能立刻看到反馈。

## 接口说明

设备侧（固件调用的）：

```
POST /report    体: {"device","version","temperature","humidity","rssi","uptime","lamp"}
                响应: {"ok":true}   只有 2xx 才会被固件认作成功

GET  /lampcmd   响应: {"lamp":"ON"} / {"lamp":"OFF"} / 204 No Content
                204 表示"没有命令"，设备什么都不做
                固件也认 cmd / value / state 三个键名
```

控制台侧（浏览器调用的）：

```
GET  /                 仪表盘页面
GET  /api/latest       最新一条上报
GET  /api/history?limit=200
GET  /api/stats        计数 + 最后上报时间
POST /api/lamp         {"state":"ON"|"OFF"|"NONE"}  排队一条命令
POST /api/clear        清空历史
```

## 数据与日志

- `telemetry.db` — SQLite，历史数据。删掉它就等于重置。
- `server.log` — 服务端 stdout，包含每条上报和每个下发的命令。
- `server.pid` — 运行中的 PID，`stop.bat` 靠它精确停止。

## 常见问题

**start.bat 说端口被占用**
```
netstat -ano | findstr :8080
taskkill /F /PID <上面查到的PID>
```

**浏览器打不开但设备能上报**
`localhost:8080` 只能本机访问，检查防火墙是否拦了 8080 的入站；
设备走的是局域网 IP，需要放行。

**设备串口报 `POST ... failed` / `HTTP 000`**
先确认 `rest_host` 是不是电脑的真实局域网 IP。用手机浏览器访问
`http://<那个IP>:8080/` 试一下——手机能打开但设备不能，就是防火墙的问题。

**想模拟"服务器版本更高"来测 OTA**
和这个后台无关，OTA 走的是 `OTA_CHECK_URL` 指向的独立 JSON 清单接口，
见工程 `main/ota_update.c`。
