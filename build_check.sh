#!/bin/bash
# 在 WSL 里编译本项目做语法/链接检查。
# 用法: wsl -d Ubuntu -- bash /mnt/d/.../build_check.sh [clean]
set -u

SRC=/mnt/d/ESP32-IDF/v5.3.2/esp-idf/examples/get-started/MPU_AWS_mqtt_REST_API
WORK=/tmp/awsrest_verify

# 独立工作目录：/mnt/d 上直接 build 会和 Windows 侧的 build/ 冲突
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$SRC"/. "$WORK"/
rm -rf "$WORK"/build

. ~/esp/esp-idf/export.sh >/dev/null 2>&1
cd "$WORK" || exit 1
idf.py build 2>&1 | tail -60
echo "=== EXIT: ${PIPESTATUS[0]} ==="
