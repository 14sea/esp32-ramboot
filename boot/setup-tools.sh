#!/bin/sh
# 免 root 安裝主機側工具：apt-get download 取 .deb，dpkg -x 解到 tools/root（不動系統）。
# 給的是：busybox-static（放進 Stage-1）、syslinux（傳統 BIOS 的引導）、QEMU + OVMF（測試用）。Debian/Ubuntu 上用；別的發行版自己裝好後把 tools/root 指過去。
# 另外需要系統裡已有：gcc make flex bison bc xz cpio mtools dosfstools gdisk e2fsprogs openssh-client python3
set -e
cd "$(dirname "$0")"
PKGS="busybox-static syslinux syslinux-common qemu-system-x86 qemu-system-common qemu-system-data ovmf seabios ipxe-qemu"
mkdir -p dl tools/root
for p in $PKGS; do
    ls dl/${p}_*.deb >/dev/null 2>&1 || (cd dl && apt-get download "$p")
    dpkg -x "$(ls dl/${p}_*.deb | tail -1)" tools/root
done
miss=
for t in gcc make flex bison bc xz cpio mcopy mkfs.fat sgdisk sfdisk mke2fs ssh-keygen python3; do
    command -v $t >/dev/null 2>&1 || [ -x /usr/sbin/$t ] || [ -x /sbin/$t ] || miss="$miss $t"
done
[ -z "$miss" ] || { echo "系統裡缺：$miss"; exit 1; }
echo "tools/root 就緒：$(tools/root/usr/bin/qemu-system-x86_64 --version | head -1)；$(tools/root/usr/bin/busybox | head -1)"
