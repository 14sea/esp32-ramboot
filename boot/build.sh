#!/bin/sh
# 從頭生成啟動盤鏡像 out/stage1.img（之後由 scripts/flash_*.sh 寫進設備端的 bootimg 分區）。
#
# 用法：./build.sh [第二階段的基地址]
#   不給地址：Stage-1 從 Ubuntu 官方源下載第二階段（實機用，目標機要能上外網）
#   ./build.sh http://10.0.2.2:8000/ubuntu ：從 run-qemu.sh 起的本地 HTTP 服務下載（QEMU 測試用）
#   換成自己的鏡像站：照官方目錄結構擺（squashfs 在頂層，內核和 initrd 在 unpacked/ 下）
# 前提：./setup-tools.sh 和 ./fetch-ubuntu.sh 跑過；內核源碼包在 dl/linux-$KVER.tar.xz
#       （https://cdn.kernel.org/pub/linux/kernel/v6.x/），或用 KERNEL_TARBALL= 指到別處。
set -e
cd "$(dirname "$0")"
PATH=$PATH:/usr/sbin:/sbin
KVER=6.18.60
K=build/linux-$KVER
[ -x tools/root/usr/bin/busybox ] || { echo "先跑 ./setup-tools.sh"; exit 1; }
[ -s ubuntu/resolute-server-cloudimg-amd64.squashfs ] || { echo "先跑 ./fetch-ubuntu.sh"; exit 1; }

if [ ! -d $K ]; then
    T=${KERNEL_TARBALL:-dl/linux-$KVER.tar.xz}
    [ -f "$T" ] || { echo "沒有內核源碼包 $T（從 https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz 下載）"; exit 1; }
    echo "==> 解壓 $T"
    mkdir -p build && tar -C build -xf "$T"
fi

echo "==> Stage-1 用戶態"
gcc -static -Os -s -o stage1/s1tool stage1/s1tool.c
stage1/mkinitramfs.sh

echo "==> Stage-1 內核（第一次約 2 分鐘）"
sed "s|@INITRAMFS_LIST@|$PWD/stage1/initramfs.list|" stage1/kernel.config > build/stage1.config
( cd $K
  [ -f .config ] && cmp -s ../stage1.config .stage1.config.applied || {
      make -s ARCH=x86_64 tinyconfig
      ./scripts/kconfig/merge_config.sh -m .config ../stage1.config >/dev/null
      make -s ARCH=x86_64 olddefconfig
      cp ../stage1.config .stage1.config.applied
  }
  make -s ARCH=x86_64 -j"$(nproc)" bzImage )

echo "==> 第二階段的配置"
stage2/mkstage2.sh "$@"
echo "==> 鏡像"
stage1/mkimage.sh 8
[ -f out/fakedisk.img ] || ./mkfakedisk.sh
