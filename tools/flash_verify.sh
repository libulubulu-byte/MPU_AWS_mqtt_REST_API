#!/bin/bash
# 在 WSL Ubuntu 里把 /tmp/awsrest_verify 的构建产物烧到 /dev/ttyUSB0。
#
# 为什么要独立工作目录：/mnt/d 上的 build/ 是 Windows 侧 idf5.4_py3.8 环境
# 生成的，和 WSL 里 export.sh 选出的 py3.11 不兼容，直接在原目录 build 会
# 报 "Run 'idf.py fullclean'"。所以构建/烧写都在 /tmp/awsrest_verify 里做。
#
# 用法: wsl -d Ubuntu -u root -- bash .../tools/flash_verify.sh
set -u

WORK=/tmp/awsrest_verify
PORT=/dev/ttyUSB0

[ -e "$PORT" ] || { echo "ERROR: $PORT 不存在（usbipd attach + modprobe ch341）"; exit 1; }

cd "$WORK" || exit 1
if [ ! -f build/aws_mqtt.bin ]; then
    echo "ERROR: build/aws_mqtt.bin 不存在，先跑 build_check.sh"
    exit 1
fi

# ⚠️ 不要走 `. export.sh`：在 root shell 里它选不出 python 环境（ESP-IDF
# 的 python venv 是普通用户 lizhen 建的，export.sh 只在它的上下文里生效），
# 结果 python 落到 /usr/bin/python 上，报 "No module named esptool"。
# 直接用 venv 里的 esptool.py 更省事也更确定。
ESPTOOL=/home/lizhen/.espressif/python_env/idf5.4_py3.11_env/bin/esptool.py
[ -x "$ESPTOOL" ] || { echo "ERROR: $ESPTOOL 不存在"; exit 1; }

# 只烧这三段：bootloader / 分区表 / 应用。ota_data_initial 会重置 OTA 状态，
# 没必要每次都写。
"$ESPTOOL" --chip esp32s3 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x0      build/bootloader/bootloader.bin \
    0x8000   build/partition_table/partition-table.bin \
    0x20000  build/aws_mqtt.bin
echo "=== FLASH EXIT: $? ==="
