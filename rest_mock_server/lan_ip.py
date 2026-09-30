#!/usr/bin/env python3
"""列出可作为设备 rest_host 的候选 IP。

排除掉设备连不上的那些：回环、APIPA(169.254)、WSL/虚拟网卡的
198.18/198.19 网段、Docker 的 172.17+，避免把一堆地址丢给用户挑。
"""
import socket

# 网段黑名单：这些地址虽然在网卡上，但 ESP32 基本连不到
BAD_PREFIXES = (
    "127.",       # 回环
    "169.254.",   # APIPA / link-local，没拿到 DHCP 时的地址
    "198.18.",    # WSL 镜像网络的虚拟网段
    "198.19.",
    "172.17.",    # Docker 默认 bridge
)

candidates = []
try:
    infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    for info in infos:
        ip = info[4][0]
        if ip not in candidates:
            candidates.append(ip)
except socket.gaierror:
    pass

# getaddrinfo 有时只给回环，补一次"连到外网时本机用哪个地址"的探测
if not candidates or all(c.startswith("127.") for c in candidates):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))   # 不真发包，只为让内核选出出口网卡
        candidates.append(s.getsockname()[0])
    except OSError:
        pass
    finally:
        s.close()

good = [ip for ip in candidates if not ip.startswith(BAD_PREFIXES)]

for ip in good:
    print(ip)
