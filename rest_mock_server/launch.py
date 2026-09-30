#!/usr/bin/env python3
"""后台启动 server.py，写日志与 PID 文件。

单独成文件是为了避开 start.bat 里内联 Python 的路径转义坑
（Windows 路径以反斜杠结尾时，r'...\\' 会吃掉收尾引号）。
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "server.log")
PID = os.path.join(HERE, "server.pid")
SERVER = os.path.join(HERE, "server.py")

# DETACHED_PROCESS 让服务脱离父控制台独立存活，
# CREATE_NEW_PROCESS_GROUP 便于将来按组结束整棵进程树。
flags = 0x00000008 | 0x00000200

with open(LOG, "ab", buffering=0) as f:
    proc = subprocess.Popen(
        [sys.executable, "-u", SERVER],
        stdout=f,
        stderr=f,
        cwd=HERE,
        creationflags=flags,
    )

with open(PID, "w") as f:
    f.write(str(proc.pid))

print(proc.pid)
