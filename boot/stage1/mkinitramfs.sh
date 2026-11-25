#!/bin/sh
# 生成給內核 CONFIG_INITRAMFS_SOURCE 用的清單（gen_init_cpio 格式），不需要 root。
# 清單裡是絕對路徑，所以 initramfs.list 不進版本庫，換了目錄要重新生成（boot/build.sh 每次都會）
set -e
cd "$(dirname "$0")"
BB=../tools/root/usr/bin/busybox
APPLETS="sh mount umount cat echo ls ip udhcpc wget sha256sum awk cut sleep stat rm mkdir uname setsid cttyhack dmesg tr ifconfig ping free df reboot poweroff grep sed head tail vi sync cp mv printf true"
{
    echo "dir /dev 755 0 0"; echo "nod /dev/console 600 0 0 c 5 1"; echo "nod /dev/null 666 0 0 c 1 3"
    for d in proc sys tmp run bin etc mnt mnt/boot; do echo "dir /$d 755 0 0"; done
    echo "file /bin/busybox $(realpath $BB) 755 0 0"
    for a in $APPLETS; do echo "slink /bin/$a busybox 777 0 0"; done
    echo "file /bin/s1tool $(realpath s1tool) 755 0 0"
    echo "file /init $(realpath rootfs/init) 755 0 0"
    for f in udhcpc.sh; do echo "file /etc/$f $(realpath rootfs/etc/$f) 755 0 0"; done
    for f in stage1.version; do echo "file /etc/$f $(realpath rootfs/etc/$f) 644 0 0"; done
} > initramfs.list
echo "initramfs.list: $(wc -l < initramfs.list) entries"
