#!/bin/sh
# 直接讀設備端板子的啟動盤，和 out/stage1.img 比對並測速（WSL 上要先裝 scripts/wsl_usb_perms.sh 的 udev 規則）
cd "$(dirname "$0")"
dev=
for d in /sys/block/sd*; do
    v=$(cat $d/device/vendor 2>/dev/null); m=$(cat $d/device/model 2>/dev/null)
    case "$v $m" in *DIY*S3\ Boot*) dev=/dev/${d##*/} ;; esac
done
[ -n "$dev" ] || { echo "找不到 S3 Boot Disk（kvm.py mode 看看是不是 hid+msc / msc，lsusb -d 303a:）"; exit 1; }
size=$(stat -c %s out/stage1.img)
echo "設備 $dev，$(cat /sys/block/${dev##*/}/size) 扇區，只讀=$(cat /sys/block/${dev##*/}/ro)"
t0=$(date +%s.%N)
got=$(dd if=$dev bs=64k iflag=direct count=$((size / 65536)) 2>/dev/null | sha256sum | cut -d' ' -f1)
t1=$(date +%s.%N)
want=$(sha256sum < out/stage1.img | cut -d' ' -f1)
awk -v s=$size -v a=$t0 -v b=$t1 'BEGIN{printf "讀 %.1f MB 用 %.1f s = %.0f KB/s\n", s/1048576, b-a, s/1024/(b-a)}'
[ "$got" = "$want" ] && echo "sha256 一致" || { echo "sha256 不一致！"; exit 1; }
