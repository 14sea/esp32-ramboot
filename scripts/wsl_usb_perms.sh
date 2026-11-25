#!/bin/sh
# 在 WSL 上讓普通用戶能直接訪問 ESP32-S3（USB VID 303a）——裝一條 udev 規則，裝一次即可，設備重插後自動生效。
# 用途：QEMU 把設備端的啟動盤直通給虛擬機（要讀寫 /dev/bus/usb 節點）；直接讀啟動盤（/dev/sdX）測速和校驗。
# 範圍：只影響廠商號 303a 的 USB 設備和它們的塊設備，別的設備權限不變。
# 用法：sudo sh scripts/wsl_usb_perms.sh        撤銷：sudo sh scripts/wsl_usb_perms.sh remove
set -e
RULE=/etc/udev/rules.d/99-esp32s3-kvm.rules

if [ "$(id -u)" != 0 ]; then
    echo "要用 sudo 跑" >&2
    exit 1
fi

if [ "$1" = remove ]; then
    rm -f "$RULE"
    udevadm control --reload
    echo "已刪除 $RULE（已接著的設備重插後恢復默認權限）"
    exit 0
fi

cat > "$RULE" <<'RULES'
# ESP32-S3 (USB VID 303a): QEMU passthrough + raw read of the boot disk. Installed by scripts/wsl_usb_perms.sh
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="303a", MODE="0666"
SUBSYSTEM=="block", SUBSYSTEMS=="usb", ATTRS{idVendor}=="303a", MODE="0666"
RULES
udevadm control --reload
udevadm trigger --subsystem-match=usb --attr-match=idVendor=303a
udevadm trigger --subsystem-match=block
udevadm settle
modprobe usb-storage 2>/dev/null || true

echo "已安裝 $RULE"
for d in /sys/bus/usb/devices/*; do
    [ "$(cat "$d/idVendor" 2>/dev/null)" = 303a ] || continue
    node=$(printf '/dev/bus/usb/%03d/%03d' "$(cat "$d/busnum")" "$(cat "$d/devnum")")
    echo "  $(cat "$d/idVendor"):$(cat "$d/idProduct") $(cat "$d/product" 2>/dev/null) → $(ls -l "$node" | cut -d' ' -f1) $node"
done
