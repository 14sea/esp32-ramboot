#!/bin/sh
# 生成給內核 CONFIG_INITRAMFS_SOURCE 用的清單（gen_init_cpio 格式），不需要 root。
# 清單裡的路徑相對內核源碼目錄（boot/build/linux-<版本>/，內核在那裡讀它），不含本機的絕對路徑
set -e
cd "$(dirname "$0")"
BB=../tools/root/usr/bin/busybox
[ -x $BB ] || { echo "沒有 $BB，先跑 ../setup-tools.sh"; exit 1; }
P=../../stage1                # 從內核源碼目錄看本目錄
APPLETS="sh mount umount cat echo ls ip udhcpc wget sha256sum awk cut sleep stat rm mkdir uname setsid cttyhack dmesg tr ifconfig ping free df reboot poweroff grep sed head tail vi sync cp mv printf true"
{
    echo "dir /dev 755 0 0"; echo "nod /dev/console 600 0 0 c 5 1"; echo "nod /dev/null 666 0 0 c 1 3"
    for d in proc sys tmp run bin etc mnt mnt/boot; do echo "dir /$d 755 0 0"; done
    echo "file /bin/busybox ../../tools/root/usr/bin/busybox 755 0 0"
    for a in $APPLETS; do echo "slink /bin/$a busybox 777 0 0"; done
    echo "file /bin/s1tool $P/s1tool 755 0 0"
    echo "file /init $P/rootfs/init 755 0 0"
    echo "file /bin/r $P/rootfs/bin/r 755 0 0"
    for f in udhcpc.sh; do echo "file /etc/$f $P/rootfs/etc/$f 755 0 0"; done
    for f in stage1.version; do echo "file /etc/$f $P/rootfs/etc/$f 644 0 0"; done
} > initramfs.list
echo "initramfs.list: $(wc -l < initramfs.list) entries"
