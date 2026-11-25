#!/bin/sh
# 測試用的假硬盤 out/fakedisk.img（256 MiB，GPT：一個 ext4 分區 + 一個 swap 分區），不需要 root。
# 用來驗證「不落盤」：系統若自動掛載 ext4 或啟用 swap，鏡像的 sha256 就會變。sha256 記在 out/fakedisk.sha256
set -e
cd "$(dirname "$0")"
PATH=$PATH:/usr/sbin:/sbin
mkdir -p out
IMG=out/fakedisk.img
dd if=/dev/zero of=$IMG bs=1M count=256 status=none
sgdisk -o -n 1:2048:264191 -t 1:8300 -n 2:264192:524254 -t 2:8200 $IMG >/dev/null
dd if=/dev/zero of=out/p1 bs=512 count=262144 status=none
mke2fs -q -t ext4 -L fakeroot out/p1
dd if=/dev/zero of=out/p2 bs=512 count=260063 status=none
mkswap -L fakeswap out/p2 >/dev/null
dd if=out/p1 of=$IMG bs=512 seek=2048 conv=notrunc status=none
dd if=out/p2 of=$IMG bs=512 seek=264192 conv=notrunc status=none
rm out/p1 out/p2
sha256sum $IMG > out/fakedisk.sha256
echo "$IMG：$(cut -c1-16 out/fakedisk.sha256)…"
