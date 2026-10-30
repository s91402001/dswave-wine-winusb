#!/bin/bash
# 錄製 Linux usbmon（給 Wine 端比對用）
#
# 用法: sudo ./record-usbmon.sh [bus] [output]
#   bus     預設 8（用 lsusb 查 2a0e:0080 的 Bus）
#   output  預設 /tmp/wine.mon
#
# 注意: 需要 root。若出現 "Operation not permitted"，請看 README 的替代方案。
set -euo pipefail

BUS="${1:-8}"
OUT="${2:-/tmp/wine.mon}"

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root：sudo $0 $BUS $OUT" >&2
    exit 1
fi

modprobe usbmon 2>/dev/null || true

if ! mountpoint -q /sys/kernel/debug; then
    mount -t debugfs none /sys/kernel/debug || true
fi

NODE="/sys/kernel/debug/usb/usbmon/${BUS}u"
if [ ! -e "$NODE" ]; then
    echo "找不到 $NODE" >&2
    echo "可用介面：" >&2
    ls /sys/kernel/debug/usb/usbmon/ 2>&1 | head >&2
    exit 1
fi

echo "錄製 $NODE -> $OUT"
echo "現在啟動 DSWave 並按執行；完成後按 Ctrl-C 停止。"
exec cat "$NODE" > "$OUT"
