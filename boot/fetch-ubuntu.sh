#!/bin/sh
# 下載第二階段用的 Ubuntu 官方文件（不改動）到 ubuntu/：squashfs 根文件系統、內核、initrd，約 390 MB。
# 已有的文件不重下。squashfs 對照官方 SHA256SUMS 校驗；三個文件的 sha256 之後都由 mkstage2.sh 釘進 manifest。
# 換版本：改這裡和 stage2/mkstage2.sh 的 SERIAL（https://cloud-images.ubuntu.com/resolute/ 下的日期目錄）
set -e
cd "$(dirname "$0")"
SERIAL=20261108
BASE=https://cloud-images.ubuntu.com/resolute/$SERIAL
N=resolute-server-cloudimg-amd64
mkdir -p ubuntu
get() { [ -s "ubuntu/$2" ] || { echo "下載 $1"; wget -q --show-progress -O "ubuntu/$2.part" "$BASE/$1" && mv "ubuntu/$2.part" "ubuntu/$2"; }; }
get SHA256SUMS SHA256SUMS
get $N.squashfs $N.squashfs
get unpacked/$N-vmlinuz-generic $N-vmlinuz-generic
get unpacked/$N-initrd-generic $N-initrd-generic
want=$(awk -v f="*$N.squashfs" '$2==f{print $1}' ubuntu/SHA256SUMS)
got=$(sha256sum < ubuntu/$N.squashfs | cut -d' ' -f1)
[ -n "$want" ] && [ "$want" = "$got" ] || { echo "squashfs 的 sha256 和 SHA256SUMS 對不上"; exit 1; }
echo "ubuntu/ 就緒（$SERIAL）：squashfs sha256 一致"
