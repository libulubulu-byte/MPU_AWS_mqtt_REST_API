#!/bin/bash
# 抓一段串口日志做闭环验证。用法: bash run_capture.sh <秒数> <日志路径>
# 例: bash run_capture.sh 180 /tmp/verify1.log
set -u
SECS=${1:-180}
OUT=${2:-/tmp/aws_capture.log}
TOOLS=/mnt/d/ESP32-IDF/v5.3.2/esp-idf/examples/get-started/MPU_AWS_mqtt_REST_API/tools

python3 "$TOOLS/serial_capture.py" --seconds "$SECS" --out "$OUT" \
    --send "45:delta {\"state\":{\"desired\":{\"lamp\":\"ON\"}}}" \
    --send "75:shadow" \
    --send "105:delta {\"state\":{\"desired\":{\"lamp\":\"OFF\"}}}" \
    --send "140:shadow"
echo "=== CAPTURE EXIT: $? -> $OUT ==="
