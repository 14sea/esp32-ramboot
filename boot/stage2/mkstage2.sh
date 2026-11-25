#!/bin/sh
# 生成放進啟動盤 /stage2/ 的小文件：manifest + hooks.cpio（dracut 鉤子）+ seed.cpio（每台機器的個性化）
# SSH 密鑰：keys/id_ed25519 沒有就生成一對，公鑰寫進 seed 的 cloud-init 配置（keys/ 不進版本庫）
# 大文件（Ubuntu 官方內核、initrd、squashfs，不改動）由 Stage-1 按 manifest 裡的地址下載，sha256 釘死在 manifest 裡。
# 用法：mkstage2.sh [基地址]
#   默認是官方源的固定日期目錄；本地測試用 http://10.0.2.2:8000/ubuntu（run-qemu.sh 會起 HTTP 服務，serve/ubuntu → ../ubuntu）
set -e
cd "$(dirname "$0")/.."
SERIAL=20261108
BASE=${1:-http://cloud-images.ubuntu.com/resolute/$SERIAL}
U=ubuntu/resolute-server-cloudimg-amd64
OUT=out/stage2-disk
rm -rf $OUT out/seed && mkdir -p $OUT serve keys
[ -f keys/id_ed25519 ] || ssh-keygen -q -t ed25519 -N "" -C ramboot -f keys/id_ed25519
cp -r stage2/seed.in out/seed
sed -i "s|@SSH_PUBKEY@|$(cat keys/id_ed25519.pub)|" out/seed/ramroot.d/var/lib/cloud/seed/nocloud/user-data
# 進系統後報告用的工具：和 Stage-1 同一個靜態二進制（build.sh 先編好）
[ -x stage1/s1tool ] || { echo "沒有 stage1/s1tool，用 ./build.sh 來跑"; exit 1; }
cp stage1/s1tool out/seed/ramroot.d/usr/local/sbin/s1tool
[ -e serve/ubuntu ] || ln -s ../ubuntu serve/ubuntu
# 官方目錄結構：squashfs 在頂層，內核和 initrd 在 unpacked/ 下；本地目錄照這個結構擺
mkdir -p ubuntu/unpacked
for f in vmlinuz-generic initrd-generic; do [ -e ubuntu/unpacked/${U##*/}-$f ] || ln $U-$f ubuntu/unpacked/; done
(cd stage2/hooks && find . | cpio -o -H newc -R 0:0 --quiet > ../../$OUT/hooks.cpio)
(cd out/seed     && find . | cpio -o -H newc -R 0:0 --quiet > ../../$OUT/seed.cpio)
sha() { sha256sum < "$1" | cut -d' ' -f1; }
{
  # @BASE@ 由 Stage-1 換成 base 那一行的地址，或板子配置裡的 base=（kvm.py cfg set base=…）
  echo "base - $BASE"
  echo "kernel $(sha $U-vmlinuz-generic) @BASE@/unpacked/${U##*/}-vmlinuz-generic"
  echo "initrd $(sha $U-initrd-generic) @BASE@/unpacked/${U##*/}-initrd-generic"
  echo "rootfs $(sha $U.squashfs) @BASE@/${U##*/}.squashfs"
  echo "initrd - hooks.cpio"
  echo "initrd - seed.cpio"
  # 不寫 root=：根由 dracut 的 mount 鉤子掛（見 hooks/）。後面幾個參數保證不碰硬盤：不組 RAID/LVM、不從休眠恢復、不自動找分區和 swap
  echo "cmdline console=ttyS0,115200 console=tty0 rd.luks=0 rd.lvm=0 rd.md=0 rd.dm=0 noresume systemd.gpt_auto=0 systemd.swap=0"
} > $OUT/manifest
ls -la $OUT | awk 'NR>3{printf "%8d  %s\n",$5,$9}'; cut -c1-150 $OUT/manifest | sed 's/[0-9a-f]\{56\}/…/'
