#!/usr/bin/env bash
# 板子直接插在本機（或經 usbipd 接進 WSL）時的燒錄。
#
# 用法：bash scripts/flash_local.sh device [full|app|img]
#       bash scripts/flash_local.sh bridge [full|app]
#   full  bootloader + 分區表 + 應用（設備端另加啟動盤鏡像）。新板子第一次燒用這個（默認）
#   app   只燒應用，NVS（藍牙綁定、開機模式）不動
#   img   只把 boot/out/stage1.img 寫進設備端的 bootimg 分區
#
# 設備端的原生 USB 口平時是鍵盤/啟動盤，要先進下載模式才有串口：
#   - 橋在線時：python3 tools/kvm.py dfu
#   - 否則：按住 BOOT 上電（或按住 BOOT 點一下 RST）
# 經 usbipd 時，進下載模式後 Windows 上會出現新設備 303a:1001，要 attach 到 WSL。
# 也可以改用板子的另一個口（USB 轉串口芯片）：PORT=/dev/ttyACM0 bash scripts/flash_local.sh device
set -euo pipefail
WHICH="${1:?用法：$0 <device|bridge> [full|app|img]}"
WHAT="${2:-full}"
. "$(dirname "$0")/common.sh"
load_idf
cd "$ROOT"

case "$WHICH" in
    device) mac=$DEVICE_MAC; proj=device ;;
    bridge) mac=$BRIDGE_MAC; proj=bridge ;;
    *) echo "用法：$0 <device|bridge> [full|app|img]"; exit 1 ;;
esac
B=firmware/$proj/build

if [ -n "${PORT:-}" ]; then
    port=$PORT; reset=(--before default_reset --after hard_reset)          # USB 轉串口芯片：自動進下載模式
elif port=$(local_port "$mac"); then
    if [ "$WHICH" = device ]; then reset=(--before no_reset --after watchdog_reset)   # 已經手動/經 dfu 進了下載模式
    else reset=(--before default_reset --after hard_reset); fi
else
    echo "找不到 $WHICH 的串口（序列號 $mac）。現在接著的 303a 設備："
    lsusb -d 303a: || true
    ls /dev/serial/by-id/ 2>/dev/null || true
    exit 1
fi
echo "==> $WHICH：$port"

args=()
if [ "$WHAT" = full ] || [ "$WHAT" = app ]; then
    (cd firmware/$proj && idf.py build 2>&1 | grep -E "error|binary size|build complete" || true)
    [ "$WHAT" = full ] && args+=(0x0 $B/bootloader/bootloader.bin 0x8000 $B/partition_table/partition-table.bin)
    args+=(0x10000 $B/$proj.bin)
fi
if [ "$WHICH" = device ] && { [ "$WHAT" = full ] || [ "$WHAT" = img ]; }; then
    [ -f "$IMG" ] || { echo "沒有 $IMG，先跑 boot/build.sh"; exit 1; }
    args+=($IMG_OFFSET "$IMG")
fi
[ ${#args[@]} -gt 0 ] || { echo "用法：$0 <device|bridge> [full|app|img]"; exit 1; }

# 經 usbip 不要用高波特率（921600 會 Corrupt data）
python -m esptool --chip esp32s3 -p "$port" "${reset[@]}" \
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m "${args[@]}"
echo "==> 燒完。經 usbipd 時板子重啟後是新的 USB 設備，Windows 上要重新 attach 到 WSL。"
