#!/bin/sh
# 啟動盤鏡像：GPT + 一個 FAT 的 EFI 分區：/EFI/BOOT/BOOTX64.EFI（= Stage-1 內核，自帶 EFI stub）+ /stage2/（第二階段的配置）
# 用法：mkimage.sh [鏡像大小MiB，默認 8] → out/stage1.img（512 字節扇區，只讀使用）
set -e
cd "$(dirname "$0")/.."
MB=${1:-8}
K=build/linux-6.18.60/arch/x86/boot/bzImage
mkdir -p out
IMG=out/stage1.img
SECT=$((MB * 2048))
PSTART=2048
PEND=$((SECT - 34 - 1))
PSECT=$((PEND - PSTART + 1))
rm -f $IMG out/esp.part
dd if=/dev/zero of=out/esp.part bs=512 count=$PSECT status=none
mkfs.fat -F 16 -s 2 -n STAGE1 -S 512 out/esp.part >/dev/null
mmd -i out/esp.part ::/EFI ::/EFI/BOOT
mcopy -i out/esp.part $K ::/EFI/BOOT/BOOTX64.EFI
# /stage2/：Stage-1 啟動後拷進內存的配置（stage2/mkstage2.sh 生成）
if [ -d out/stage2-disk ]; then
    mmd -i out/esp.part ::/stage2
    mcopy -i out/esp.part out/stage2-disk/* ::/stage2/
fi
dd if=/dev/zero of=$IMG bs=512 count=$SECT status=none
sgdisk -o -n 1:$PSTART:$PEND -t 1:ef00 -c 1:STAGE1 $IMG >/dev/null
dd if=out/esp.part of=$IMG bs=512 seek=$PSTART conv=notrunc status=none
rm out/esp.part
echo "kernel: $(stat -c %s $K) bytes;  image: $(stat -c %s $IMG) bytes ($MB MiB)"
mdir -i $IMG@@$((PSTART * 512)) -/ :: | grep -E "EFI  |cpio|manifest|bytes free"
