#!/usr/bin/env bash
set -euo pipefail

echo "检查基础命令..."

need_cmd() {
  local cmd="$1"
  if command -v "$cmd" >/dev/null 2>&1; then
    echo "OK: $cmd"
  else
    echo "ERROR: 缺少命令 $cmd"
    exit 1
  fi
}

need_cmd git
need_cmd python3
need_cmd cmake
need_cmd ninja

echo
echo "检查 ESP-IDF..."
if command -v idf.py >/dev/null 2>&1; then
  echo "OK: idf.py"
  idf.py --version || true
else
  echo "WARN: 当前 shell 中未找到 idf.py"
  echo "      如已安装 ESP-IDF，请先执行: source ~/esp/esp-idf/export.sh"
fi

echo
echo "检查串口..."
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "WARN: 未发现 ttyUSB/ttyACM 设备"

echo
echo "检查工程目录..."
test -d main/boards/otto-robot && echo "OK: main/boards/otto-robot"
test -d main/boards/common && echo "OK: main/boards/common"
test -f main/boards/otto-robot/config.h && echo "OK: otto config.h"
test -f main/boards/otto-robot/config.json && echo "OK: otto config.json"

echo
echo "检查完成。"
echo "推荐下一步："
echo "  1. source ~/esp/esp-idf/export.sh"
echo "  2. idf.py set-target esp32s3"
echo "  3. idf.py fullclean"
echo "  4. idf.py menuconfig"
echo "  5. idf.py -DBOARD_NAME=otto-robot build"
