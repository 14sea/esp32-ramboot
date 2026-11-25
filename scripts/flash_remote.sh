#!/usr/bin/env bash
# 兩塊板子都插在另一台 Linux 機器（boards.env 的 REMOTE_HOST）上時的燒錄。遠端不需要 sudo（用戶在 dialout 組即可）。
#
# 用法：KVM_PORT=socket://127.0.0.1:15555 bash scripts/flash_remote.sh <device|bridge|both> [img]
#   device  燒設備端的應用（經橋發 dfu 進下載模式，所以橋必須在線、tools/serial_relay.py 的隧道必須通）
#   bridge  燒橋的應用。會先停掉遠端的 serial_relay.py（它佔著橋的串口），燒完要自己重開
#   both    先 device 後 bridge（兩邊協議一起改的時候用）
#   img     另外把 boot/out/stage1.img 寫進設備端的 bootimg 分區
# 只寫應用分區（0x10000），不動分區表和 NVS。先在本機編譯好（idf.py build）。
set -euo pipefail
WHAT="${1:?用法：$0 <device|bridge|both> [img]}"
WITH_IMG="${2:-}"
. "$(dirname "$0")/common.sh"
HOST="${REMOTE_HOST:?boards.env 裡沒有 REMOTE_HOST}"
rsh() { ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST" "$@"; }
byid() { rsh "ls /dev/serial/by-id/ 2>/dev/null | grep -i 'Espressif_USB_JTAG.*$1' | head -1" || true; }
# 不用遠端系統自帶的 esptool：舊版本沒有 watchdog_reset，燒完不會復位
esptool() { rsh "cd /tmp/kvmfw && PYTHONPATH=/tmp/esptool-idf python3 -m esptool --chip esp32s3 $*" | grep -E "Wrote|rror|fatal|MAC:" | cut -c1-70; }
load_idf

echo "==> 拷固件和 IDF 的 esptool 到 $HOST"
SITE="$(python -c 'import esptool, os; print(os.path.dirname(os.path.dirname(esptool.__file__)))')"
rsh "mkdir -p /tmp/kvmfw /tmp/esptool-idf"
rsync -a --exclude __pycache__ "$SITE/esptool" "$SITE/intelhex" "$HOST:/tmp/esptool-idf/"
rsync -a "$ROOT/firmware/device/build/device.bin" "$ROOT/firmware/bridge/build/bridge.bin" "$HOST:/tmp/kvmfw/"
[ "$WITH_IMG" = img ] && rsync -a "$IMG" "$HOST:/tmp/kvmfw/"

if [ "$WHAT" = device ] || [ "$WHAT" = both ]; then
    if [ -z "$(byid "$DEVICE_MAC")" ]; then
        echo "==> 設備端：經橋發 dfu"
        python3 "$ROOT/tools/kvm.py" --wait 15 dfu | tail -1
    fi
    port=""
    for _ in $(seq 30); do port="$(byid "$DEVICE_MAC")"; [ -n "$port" ] && break; sleep 1; done
    [ -n "$port" ] || { echo "沒等到設備端的下載口"; exit 1; }
    echo "==> 燒設備端（/dev/serial/by-id/$port）"
    extra=""; [ "$WITH_IMG" = img ] && extra="$IMG_OFFSET stage1.img"
    esptool "-p /dev/serial/by-id/$port --before no_reset --after watchdog_reset write_flash 0x10000 device.bin $extra"
fi

if [ "$WHAT" = bridge ] || [ "$WHAT" = both ]; then
    # 方括號寫法：pgrep/pkill -f 不會匹配到自己這條命令行
    if [ -n "$(rsh "pgrep -f '[s]erial_relay.py'" || true)" ]; then
        echo "==> 停掉遠端的 serial_relay.py（它佔著橋的串口）"
        rsh "pkill -f '[s]erial_relay.py'" || true
        sleep 1
    fi
    port="$(byid "$BRIDGE_MAC")"
    [ -n "$port" ] || { echo "找不到橋的串口"; exit 1; }
    echo "==> 燒橋（/dev/serial/by-id/$port）"
    esptool "-p /dev/serial/by-id/$port --after hard_reset write_flash 0x10000 bridge.bin"
    echo "    燒完了。記得重開 serial_relay.py 和 SSH 隧道"
fi
sleep 4
echo "==> 遠端現在的設備：$(rsh "lsusb -d 303a: | awk '{print \$6}'" | tr '\n' ' ')"
