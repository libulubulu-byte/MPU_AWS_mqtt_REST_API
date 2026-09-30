#!/usr/bin/env python3
"""aws_check.py - 用 PC 上的 Python 验证 AWS IoT Core 能不能连上。

为什么要有这个脚本：证书/端点配错时 ESP32 只会打印一句含糊的
"MQTT_EVENT_ERROR / TLS connect error"，看不出到底是证书错了、端点错了还是
策略没放行。先在这里跑通，能把"配置问题"和"固件问题"彻底分开。

它直接从 main/aws_certs.h 里把三份 PEM 抽出来（不做任何手工复制），验证：
  1. 私钥与设备证书是否配对          （签发证书时最常见的错配）
  2. 证书有没有过期 / 是不是还没生效
  3. 能不能完成 TLS 双向认证并 CONNECT 到 8883
  4. 能不能往 <thing>/esp32s3-sht30/state 发布一条测试消息
  5. 能不能订阅并收到 <thing>/esp32s3-sht30/lamp/set（顺便验证策略是否放行）

用法：
    pip install paho-mqtt cryptography
    python tools/aws_check.py                # 用 aws_certs.h 里的端点/Thing
    python tools/aws_check.py --endpoint xxx-ats.iot.ap-southeast-2.amazonaws.com

退出码 0 = 全部通过。
"""
import argparse
import datetime
import os
import re
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CERTS_H = os.path.join(os.path.dirname(HERE), "main", "aws_certs.h")

MACROS = {
    "AWS_IOT_CA_PEM": "根 CA",
    "AWS_IOT_CERT_PEM": "设备证书",
    "AWS_IOT_KEY_PEM": "设备私钥",
}


def parse_header(path):
    """把 aws_certs.h 里的 #define XXX "a\\n" "b\\n" 拼回完整 PEM，
    并顺手读出 AWS_IOT_ENDPOINT / _PORT / _THING。"""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    out = {}
    for name in MACROS:
        m = re.search(
            r'#define\s+' + name + r'\s*(.*?)(?=\n#|\n/\*|\Z)', text, re.S)
        if not m:
            raise SystemExit("在 %s 里找不到 #define %s" % (path, name))
        body = m.group(1)
        # 拼所有字符串字面量
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        if not parts:
            raise SystemExit("%s 里没有字符串字面量" % name)
        pem = "".join(parts)
        # 解码 C 转义：这里只可能出现 \n（PEM 里没有引号和反斜杠）
        pem = pem.replace("\\n", "\n").replace("\\\\", "\\")
        if "REPLACE_WITH_" in pem:
            raise SystemExit("%s 还是占位符，先从 AWS 控制台把证书填进 %s"
                             % (name, path))
        out[name] = pem

    def simple(name, default):
        m = re.search(r'#define\s+' + name + r'\s+"([^"]*)"', text)
        return m.group(1) if m else default

    out["endpoint"] = simple("AWS_IOT_ENDPOINT", "")
    m = re.search(r'#define\s+AWS_IOT_PORT\s+(\d+)', text)
    out["port"] = int(m.group(1)) if m else 8883
    out["thing"] = simple("AWS_IOT_THING", "ESP32S3")

    # 主题基名真正的来源是 Kconfig（sdkconfig 里的 CONFIG_APP_AWS_BASE_TOPIC）。
    # 取不到就按默认值拼一个，保证脚本行为与固件一致。
    base = None
    for cand in (os.path.join(os.path.dirname(CERTS_H), "..", "sdkconfig"),):
        if os.path.isfile(cand):
            with open(cand, encoding="utf-8", errors="replace") as f:
                mm = re.search(r'^CONFIG_APP_AWS_BASE_TOPIC="([^"]*)"', f.read(),
                               re.M)
            if mm:
                base = mm.group(1)
            break
    out["base"] = base or ("%s/esp32s3-sht30" % out["thing"])

    # 主机名/端口也可能写在 tools/ 同级的 config；这里只认头文件
    return out


def check_key_matches_cert(key_pem, cert_pem):
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization

    try:
        cert = x509.load_pem_x509_certificate(cert_pem.encode())
        key = serialization.load_pem_private_key(key_pem.encode(), password=None)
    except Exception as e:                       # noqa: BLE001
        return False, "解析失败：%s" % e

    a = cert.public_key().public_numbers()
    b = key.public_key().public_numbers()
    if (a.n, a.e) != (b.n, b.e):
        return False, ("私钥和证书不配对 —— 这两份来自不同的 thing，"
                       "AWS 会直接断连")

    now = datetime.datetime.now(datetime.timezone.utc)
    if now < cert.not_valid_before_utc:
        return False, "证书还没生效（notBefore=%s）" % cert.not_valid_before_utc
    if now > cert.not_valid_after_utc:
        return False, "证书已过期（%s）" % cert.not_valid_after_utc

    subj = cert.subject.rfc4514_string()
    return True, ("配对 OK，有效期至 %s，subject=%s"
                  % (cert.not_valid_after_utc, subj))


def tcp_reachable(host, port, timeout=8):
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            peer = s.getpeername()
        return True, "TCP 可达 %s:%d" % (peer[0], peer[1])
    except OSError as e:
        return False, "TCP 连不上 %s:%d -> %s（查端点拼写/区域/网络代理）" % (
            host, port, e)


def force_alpn(cli, proto):
    """paho-mqtt 没有暴露 ALPN 接口，只能在它复用的 SSLContext 上设。
    AWS IoT Core 要求 ALPN=x-amzn-mqtt-ca，缺了它服务端会在握手最后直接
    断连（EOF occurred in violation of protocol），且不给任何有用信息。"""
    ctx = cli._ssl_context
    if ctx is None:
        raise SystemExit("paho 版本不支持：_ssl_context 为空，"
                         "请确认先调用了 tls_set()")
    ctx.set_alpn_protocols([proto])      # 注意是 protocols（复数）
    return ctx


def mqtt_roundtrip(cfg, ca_file, cert_file, key_file, timeout=20):
    import paho.mqtt.client as mqtt

    base = cfg["base"]
    set_topic = base + "/lamp/set"
    state_topic = base + "/state"

    got = {}
    done = threading.Event()

    cli = mqtt.Client(client_id="pc-check-%d" % int(time.time()),
                      protocol=mqtt.MQTTv311)
    cli.tls_set(ca_certs=ca_file, certfile=cert_file, keyfile=key_file,
                tls_version=__import__("ssl").PROTOCOL_TLS_CLIENT)
    cli.tls_insecure_set(False)
    force_alpn(cli, "x-amzn-mqtt-ca")

    def on_connect(c, userdata, flags, rc, props=None):
        got["connect_rc"] = rc
        if rc == 0:
            c.subscribe(set_topic, qos=1)
            c.publish(state_topic, '{"pc_check":"hello"}', qos=1)
        else:
            done.set()

    def on_subscribe(c, userdata, mid, granted_qos, props=None):
        got["subscribed"] = True
        c.publish(set_topic, "OFF", qos=1)   # 自己发给自己，验证订阅通道
        c.publish(state_topic, '{"pc_check":"roundtrip"}', qos=1)

    def on_message(c, userdata, msg):
        got.setdefault("msgs", []).append((msg.topic, msg.payload.decode()))
        if msg.topic == set_topic:
            done.set()

    def on_disconnect(c, userdata, rc, props=None):
        got["disconnect_rc"] = rc

    cli.on_connect = on_connect
    cli.on_subscribe = on_subscribe
    cli.on_message = on_message
    cli.on_disconnect = on_disconnect

    # paho 2.x 用回调 API 版本参数；1.x 没有这个属性，做兼容
    try:
        cli._client = None
    except Exception:                            # noqa: BLE001
        pass

    try:
        cli.connect(cfg["endpoint"], cfg["port"], keepalive=30)
    except Exception as e:                       # noqa: BLE001
        return False, "connect() 失败：%s" % e

    cli.loop_start()
    ok = done.wait(timeout)
    time.sleep(0.5)
    cli.loop_stop()
    try:
        cli.disconnect()
    except Exception:                            # noqa: BLE001
        pass

    rc = got.get("connect_rc")
    if rc is None:
        return False, ("TLS 握手阶段就失败了（没有收到 CONNACK）。"
                       "常见原因：证书/私钥不配对、时钟不对、ALPN 未协商、"
                       "或本机被防火墙拦")
    if rc != 0:
        meanings = {1: "协议版本不支持", 2: "Client ID 被拒绝",
                    3: "服务不可用", 4: "用户名/密码错误",
                    5: "未授权（策略或证书未关联 thing）"}
        return False, "CONNACK rc=%d %s" % (rc, meanings.get(rc, ""))
    if not ok:
        return False, ("已连上但没等到自己发的 lamp/set 消息 —— "
                       "多半是 IoT 策略没放行 %s 的订阅" % set_topic)

    return True, ("已连接，订阅+发布往返成功，收到 %d 条消息：%s"
                  % (len(got.get("msgs", [])), got.get("msgs")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endpoint", help="覆盖 aws_certs.h 里的端点")
    ap.add_argument("--thing", help="覆盖 aws_certs.h 里的 Thing 名")
    ap.add_argument("--port", type=int)
    ap.add_argument("--skip-mqtt", action="store_true",
                    help="只做本地证书自检，不联网")
    args = ap.parse_args()

    cfg = parse_header(CERTS_H)
    if args.endpoint:
        cfg["endpoint"] = args.endpoint
    if args.thing:
        cfg["thing"] = args.thing
    if args.port:
        cfg["port"] = args.port

    print("=" * 68)
    print("端点    : %s:%d" % (cfg["endpoint"], cfg["port"]))
    print("Thing   : %s" % cfg["thing"])
    print("主题前缀: %s" % cfg["base"])
    print("=" * 68)

    fail = 0

    print("\n[1/3] 本地证书自检")
    ok, msg = check_key_matches_cert(cfg["AWS_IOT_KEY_PEM"],
                                    cfg["AWS_IOT_CERT_PEM"])
    print("  %s %s" % ("PASS" if ok else "FAIL", msg))
    fail += 0 if ok else 1

    print("\n[2/3] TCP 连通性")
    ok, msg = tcp_reachable(cfg["endpoint"], cfg["port"])
    print("  %s %s" % ("PASS" if ok else "FAIL", msg))
    fail += 0 if ok else 1

    if args.skip_mqtt or fail:
        print("\n跳过 MQTT 测试" + ("" if args.skip_mqtt else "（前面有失败）"))
    else:
        print("\n[3/3] MQTT over TLS 双向认证 + 收发")
        # paho 要文件路径，写到临时文件
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            ca = os.path.join(d, "ca.pem")
            crt = os.path.join(d, "cert.pem.crt")
            key = os.path.join(d, "private.pem.key")
            for p, content in ((ca, cfg["AWS_IOT_CA_PEM"]),
                               (crt, cfg["AWS_IOT_CERT_PEM"]),
                               (key, cfg["AWS_IOT_KEY_PEM"])):
                with open(p, "w", encoding="utf-8") as f:
                    f.write(content)
            ok, msg = mqtt_roundtrip(cfg, ca, crt, key)
        print("  %s %s" % ("PASS" if ok else "FAIL", msg))
        fail += 0 if ok else 1

    print("\n" + "=" * 68)
    print("结果：%s" % ("全部通过，ESP32 那边配置照抄即可" if fail == 0
                    else "%d 项失败" % fail))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
