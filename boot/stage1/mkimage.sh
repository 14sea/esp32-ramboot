#!/bin/sh
# 啟動盤鏡像，UEFI 和傳統 BIOS 都能啟動：
#   MBR 分區表 + 一個活動的 FAT16 分區。老 BIOS 的 USB 啟動只認 MBR + 活動分區；UEFI 對 MBR 分區的 FAT 盤同樣認
#   UEFI：  /EFI/BOOT/BOOTX64.EFI（= Stage-1 內核，自帶 EFI stub）
#   BIOS：  MBR 引導碼（syslinux 的 mbr.bin）→ 分區引導扇區 → ldlinux.sys → syslinux.cfg → 同一個內核文件
#   /stage2/：Stage-1 啟動後拷進內存的配置（stage2/mkstage2.sh 生成）
# 用法：mkimage.sh [鏡像大小MiB，默認 8] → out/stage1.img（512 字節扇區，只讀使用）
set -e
cd "$(dirname "$0")/.."
PATH=$PATH:/usr/sbin:/sbin
R=$PWD/tools/root
MB=${1:-8}
K=build/linux-6.18.60/arch/x86/boot/bzImage
mkdir -p out
IMG=out/stage1.img
SECT=$((MB * 2048))
PSTART=2048
PSECT=$((SECT - PSTART))
rm -f $IMG out/esp.part
dd if=/dev/zero of=out/esp.part bs=512 count=$PSECT status=none
# -h：隱藏扇區數 = 分區起始扇區，分區引導扇區靠它找到自己在盤上的位置（傳統 BIOS 啟動必需）
mkfs.fat -F 16 -s 2 -n STAGE1 -S 512 -h $PSTART out/esp.part >/dev/null
mmd -i out/esp.part ::/EFI ::/EFI/BOOT
mcopy -i out/esp.part $K ::/EFI/BOOT/BOOTX64.EFI
if [ -d out/stage2-disk ]; then
    mmd -i out/esp.part ::/stage2
    mcopy -i out/esp.part out/stage2-disk/* ::/stage2/
fi
# 傳統 BIOS：syslinux 裝進分區（寫引導扇區和 ldlinux.sys / ldlinux.c32），配置指向同一個內核文件
printf 'DEFAULT s1\nPROMPT 0\nTIMEOUT 0\nSERIAL 0 115200\nLABEL s1\n  LINUX /EFI/BOOT/BOOTX64.EFI\n' > out/syslinux.cfg
mcopy -i out/esp.part out/syslinux.cfg ::/syslinux.cfg
$R/usr/bin/syslinux --install out/esp.part
dd if=/dev/zero of=$IMG bs=512 count=$SECT status=none
# 分區類型 0e = FAT16（LBA），帶活動標誌
printf 'label: dos\nstart=%d, size=%d, type=0e, bootable\n' $PSTART $PSECT | sfdisk -q $IMG
dd if=$R/usr/lib/syslinux/mbr/mbr.bin of=$IMG bs=440 count=1 conv=notrunc status=none
dd if=out/esp.part of=$IMG bs=512 seek=$PSTART conv=notrunc status=none
rm out/esp.part out/syslinux.cfg
echo "kernel: $(stat -c %s $K) bytes;  image: $(stat -c %s $IMG) bytes ($MB MiB)"
mdir -i $IMG@@$((PSTART * 512)) -/ :: | grep -E "EFI  |cpio|manifest|SYS|CFG|C32|bytes free"
