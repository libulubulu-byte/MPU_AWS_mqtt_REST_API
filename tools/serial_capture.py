#!/usr/bin/env python3
"""抓 /dev/ttyUSB0 的串口日志，并可定时注入 console 命令。

为什么必须用 pyserial 显式控制 DTR/RTS：ESP32-S3 DevKit 的自动复位电路
把 DTR/RTS 接到了 EN 和 GPIO0 上。直接 `cat /dev/ttyUSB0` 时串口一打开就
会产生 DTR/RTS 电平跳变，芯片被按进 bootloader，串口上只剩 ROM 横幅，
一句应用日志都看不到。这里显式把两个信号都拉到"正常运行"（DTR=False,
RTS=False），复位一次之后就开始收日志。

用法:
  python3 serial_capture.py --seconds 120 --out /tmp/run1.log
  python3 serial_capture.py --seconds 180 \\
      --send 40:'delta {"state":{"desired":{"lamp":"ON"}}}' \\
      --out /tmp/run_delta.log

--send 的格式是 SEC:LINE（SEC 是从开始抓取算起的秒数），会把 LINE 加回车
写进串口，用来在固定时刻触发 console 命令做闭环验证。
"""
import argparse
import sys
import threading
import time

import serial

# Windows 控制台默认是 GBK：日志里出现非 GBK 字节（串口偶发乱码）时，
# print() 会直接抛 UnicodeEncodeError 把抓取脚本打断。这里把标准输出和
# 标准错误都强制成 UTF-8，宁可看到 '?' 也不要半路崩掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--out", default=None, help="日志落盘路径（同时打到 stdout）")
    ap.add_argument("--send", action="append", default=[],
                    metavar="SEC:LINE",
                    help="到第 SEC 秒时向串口写一行（可重复）")
    ap.add_argument("--no-reset", action="store_true",
                    help="不要拉 DTR/RTS 复位，直接开始读")
    args = ap.parse_args()

    sends = []
    for spec in args.send:
        sec, _, line = spec.partition(":")
        sends.append((float(sec), line))
    sends.sort()

    out = open(args.out, "w", encoding="utf-8", errors="replace") if args.out else None

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.1

    if args.no_reset:
        # 只控流控，不动 DTR/RTS，避免打断正在跑的会话
        ser.dtr = None
        ser.rts = None

    ser.open()

    if not args.no_reset:
        # EN 复位脉冲：RTS 拉低 -> 释放。DTR 保持高（GPIO0=高 => 正常启动）。
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setRTS(False)
        time.sleep(0.05)

    print(f"# capturing {args.port} @{args.baud} for {args.seconds}s "
          f"({len(sends)} scheduled sends)", file=sys.stderr, flush=True)

    start = time.time()
    deadline = start + args.seconds
    pending = list(sends)

    buf = b""
    while time.time() < deadline:
        now = time.time() - start

        while pending and now >= pending[0][0]:
            _, line = pending.pop(0)
            ser.write((line + "\r\n").encode())
            ser.flush()
            msg = f"\n### [t={now:6.1f}s] TX >>> {line}\n"
            print(msg, end="", flush=True)
            if out:
                out.write(msg)
                out.flush()

        data = ser.read(4096)
        if not data:
            continue

        buf += data
        # 按行切分；不完整的尾巴留到下一轮，避免把一行日志截成两半
        while b"\n" in buf:
            line_bytes, buf = buf.split(b"\n", 1)
            line = line_bytes.decode("utf-8", errors="replace").rstrip("\r")
            stamp = f"[t={time.time() - start:7.2f}] {line}"
            print(stamp, flush=True)
            if out:
                out.write(stamp + "\n")
                out.flush()

    if buf:
        line = buf.decode("utf-8", errors="replace").rstrip("\r")
        print(f"[t={time.time() - start:7.2f}] {line}")
        if out:
            out.write(line + "\n")

    ser.close()
    if out:
        out.close()
    print("# capture finished", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
