#!/usr/bin/env python3
"""shadow_pub.py - 从 PC 发一条上报，验证 Shadow -> 规则 -> DynamoDB 整条链路。

为什么需要它：DynamoDB 表空着有两种完全不同的原因，光看设备日志分不出来：
  1) 规则的主题没匹配到任何消息（规则压根不触发）；
  2) 规则触发了但动作失败（表名写错、键取到空值、IAM 角色没权限）。
本脚本用**和设备同一份证书、但不同的 Client ID**从 PC 独立发一条，
把变量从"设备"换成"你能完全控制的 PC"，一次就能把上面两种原因分开。

用法：
    pip install paho-mqtt
    python tools\\shadow_pub.py                     # 发到影子的 update 主题
    python tools\\shadow_pub.py --plain test/shadow # 发到普通主题（见下方警告）

⚠️ --plain 需要 IoT 策略放行那个测试主题，否则会被静默丢弃
   （设备证书的策略通常只放行 $aws/things/<thing>/shadow/*）。
   不改策略的话，用不带参数的默认模式就够了。

⚠️ 默认模式会**临时覆盖**影子的 reported（temperature 变成 66.6 之类）。
   设备下一个上报周期会把真实值写回来，不用手工恢复。
"""
import argparse
import datetime
import json
import os
import ssl
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# 复用 aws_check.py 里那套"从 aws_certs.h 抽 PEM"的解析 —— 它已经处理了
# C 字符串转义与行长校验，另写一份迟早和它漂移。
from aws_check import CERTS_H, force_alpn, parse_header   # noqa: E402


def build_payload():
    """字段结构和设备 shadow.c 的 reported 保持一致。

    必须对得上规则的 SQL（state.reported.device / time.iso / ...），
    否则测出来的"失败"是 payload 写错，而不是规则写错。
    """
    return {
        "state": {
            "reported": {
                "device": "pc-publisher-test",
                "fw": "pc-script",
                "lamp": "OFF",
                "temperature": 66.6,
                "humidity": 77.7,
                "rssi": -40,
                "uptime": 1,
                "report_interval_s": 10,
                "time": {
                    "epoch": int(time.time()),
                    "iso": datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
                    "src": "pc",
                    "synced": True,
                    "tz": "CST-8",
                    "valid": True,
                },
                "cmd_result": "pc-test",
                "cmd_seq": 0,
            }
        }
    }


def _new_client(mqtt, client_id):
    """paho 2.x 要求显式给回调 API 版本，1.x 没有这个参数 —— 两边兼容。"""
    try:
        from paho.mqtt.enums import CallbackAPIVersion
        return mqtt.Client(CallbackAPIVersion.VERSION1, client_id=client_id,
                           protocol=mqtt.MQTTv311)
    except Exception:                                    # noqa: BLE001
        return mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)


def publish_once(cfg, ca_file, crt_file, key_file, plain_topic, wait_s):
    import paho.mqtt.client as mqtt

    thing = cfg["thing"]
    shadow_update = "$aws/things/%s/shadow/update" % thing
    accepted = "$aws/things/%s/shadow/update/accepted" % thing
    rejected = "$aws/things/%s/shadow/update/rejected" % thing

    topic = plain_topic or shadow_update
    payload = json.dumps(build_payload())

    got = {"msgs": [], "connect_rc": None}
    # Client ID 必须全局唯一：和设备的 Thing 名撞了会把设备顶下线。
    cli = _new_client(mqtt, "pc-shadow-pub-%d" % int(time.time()))
    cli.tls_set(ca_certs=ca_file, certfile=crt_file, keyfile=key_file,
                tls_version=ssl.PROTOCOL_TLS_CLIENT)
    cli.tls_insecure_set(False)
    try:
        force_alpn(cli, "x-amzn-mqtt-ca")
    except SystemExit as e:
        return False, "设置 ALPN 失败：%s" % e, got

    def on_connect(c, userdata, flags, rc, props=None):
        got["connect_rc"] = rc
        if rc != 0:
            return
        c.subscribe(plain_topic or accepted, qos=1)
        if not plain_topic:
            c.subscribe(rejected, qos=1)
        print("  已连接，发布到 %s" % topic)
        c.publish(topic, payload, qos=1)

    def on_message(c, userdata, msg):
        got["msgs"].append((msg.topic, msg.payload.decode("utf-8", "replace")))

    cli.on_connect = on_connect
    cli.on_message = on_message

    try:
        cli.connect(cfg["endpoint"], cfg["port"], keepalive=30)
    except Exception as e:                               # noqa: BLE001
        return False, "connect() 失败：%s" % e, got

    cli.loop_start()
    time.sleep(wait_s)
    cli.loop_stop()
    try:
        cli.disconnect()
    except Exception:                                    # noqa: BLE001
        pass

    rc = got["connect_rc"]
    if rc is None:
        return False, ("TLS 握手阶段就失败了（没收到 CONNACK）—— "
                       "查证书配对、ALPN、本机防火墙"), got
    if rc != 0:
        meanings = {1: "协议版本不支持", 2: "Client ID 被拒绝",
                    3: "服务不可用", 4: "用户名/密码错误",
                    5: "未授权（策略可能限定了 Client ID）"}
        return False, "CONNACK rc=%d %s" % (rc, meanings.get(rc, "")), got
    return True, "已发出", got


def main():
    ap = argparse.ArgumentParser(description="从 PC 发一条上报验证规则链路")
    ap.add_argument("--plain", metavar="TOPIC",
                    help="发到普通主题（需策略放行；配合临时改规则的 FROM）")
    ap.add_argument("--endpoint", help="覆盖 aws_certs.h 里的端点")
    ap.add_argument("--thing", help="覆盖 aws_certs.h 里的 Thing 名")
    ap.add_argument("--port", type=int)
    ap.add_argument("--wait", type=float, default=8.0,
                    help="发完等多少秒收影子回执（默认 8）")
    args = ap.parse_args()

    cfg = parse_header(CERTS_H)
    if args.endpoint:
        cfg["endpoint"] = args.endpoint
    if args.thing:
        cfg["thing"] = args.thing
    if args.port:
        cfg["port"] = args.port

    topic = args.plain or ("$aws/things/%s/shadow/update" % cfg["thing"])
    print("=" * 68)
    print("端点   : %s:%d" % (cfg["endpoint"], cfg["port"]))
    print("Thing  : %s" % cfg["thing"])
    print("发布到 : %s" % topic)
    print("=" * 68)

    with tempfile.TemporaryDirectory() as d:
        ca = os.path.join(d, "ca.pem")
        crt = os.path.join(d, "cert.pem.crt")
        key = os.path.join(d, "private.pem.key")
        for p, content in ((ca, cfg["AWS_IOT_CA_PEM"]),
                           (crt, cfg["AWS_IOT_CERT_PEM"]),
                           (key, cfg["AWS_IOT_KEY_PEM"])):
            with open(p, "w", encoding="utf-8") as f:
                f.write(content)
        ok, msg, got = publish_once(cfg, ca, crt, key, args.plain, args.wait)

    print("\n连接/发布：%s %s" % ("OK" if ok else "FAIL", msg))

    msgs = got.get("msgs", [])
    if msgs:
        print("\n收到 %d 条回执：" % len(msgs))
        for t, p in msgs:
            body = p if len(p) <= 400 else p[:400] + " ...(%d 字节)" % len(p)
            print("  <- %s" % t)
            print("     %s" % body)
    elif ok:
        print("\n没收到 accepted/rejected 回执（%s 秒内）" % args.wait)

    print("\n" + "=" * 68)
    print("下一步：等 10 秒，去 DynamoDB 表点刷新，找 device=pc-publisher-test 的行")
    if args.plain:
        print("  - 有这行  -> 规则的 SQL / 键映射 / IAM 全对，问题只在 FROM 匹配不到设备消息")
        print("  - 没有    -> 动作配置有问题：Table name、键占位符、IAM role")
        print("  ⚠️ 测完把规则的 FROM 改回 $aws/things/%s/shadow/update/accepted"
              % cfg["thing"])
    else:
        print("  - 有这行  -> 规则和主题都对，设备那边也应该开始进数据了")
        print("  - 没有    -> 规则没触发（Thing 名逐字核对）或动作失败（表名/IAM）")
    print("=" * 68)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
