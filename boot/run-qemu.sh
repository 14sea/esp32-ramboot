#!/bin/sh
# 在 QEMU + OVMF 裡跑整條鏈：UEFI 從「U 盤」啟動 Stage-1 → 下載 → kexec → 內存裡的 Ubuntu（SSH 轉發到本機 2222）
# 沒有 KVM，純軟件模擬，慢。串口輸出寫到 out/serial.log
# 環境變量：MEM=4096  NIC=e1000e|virtio-net-pci|none  REAL=<設備端的 USB PID>|port  RESETS=1
#           BIOS=1 用傳統 BIOS（SeaBIOS）代替 UEFI，測老機器的啟動方式
#           HD=<鏡像文件> 換一塊硬盤（默認 out/fakedisk.img）；NOSTICK=1 不掛啟動盤（測「裝好的硬盤自己能不能啟動」）
cd "$(dirname "$0")"
R=$PWD/tools/root
mkdir -p out
cp -f $R/usr/share/OVMF/OVMF_VARS_4M.fd out/vars.fd
# 假硬盤：用來驗證「不落盤」——跑前跑後 sha256 應該相同（build.sh 會建）
HD=${HD:-out/fakedisk.img}
[ -f "$HD" ] || { echo "沒有 $HD（默認的假硬盤由 ./build.sh 建）"; exit 1; }
# 啟動盤：默認用鏡像文件；REAL=4006（或 4002）改成直通真實的設備端板子（USB PID，見 kvm.py mode），
# 需要先裝 scripts/wsl_usb_perms.sh 的 udev 規則
if [ "$REAL" = port ]; then
    # 按 USB 口直通：板子換身分（PID 變了）之後 QEMU 會自動把新設備接回來。找當前 VID 303a、不是下載口(1001) 的那個口
    # usbip 斷線重連後 WSL 裡會留下失效的舊設備節點，取設備號最大（最新接上）的那個
    N=0
    for d in /sys/bus/usb/devices/*; do
        [ "$(cat $d/idVendor 2>/dev/null)" = 303a ] && [ "$(cat $d/idProduct)" != 1001 ] || continue
        n=$(cat $d/devnum); [ $n -gt $N ] && N=$n HB=$(cat $d/busnum) HP=$(cat $d/devpath)
    done
    [ -n "$HP" ] || { echo "找不到設備端板子（lsusb -d 303a:）"; exit 1; }
    echo "直通 USB bus=$HB port=$HP"
    STICK_ARGS="-device usb-host,bus=xhci.0,hostbus=$HB,hostport=$HP,bootindex=1,guest-resets-all=on"
    # ALSO_IMG=1：另外再掛一塊鏡像文件當啟動盤（板子當前模式沒有啟動盤時用）
    [ -n "$ALSO_IMG" ] && STICK_ARGS="$STICK_ARGS -drive if=none,id=stick,format=raw,readonly=on,file=out/stage1.img -device usb-storage,bus=xhci.0,drive=stick,bootindex=2"
elif [ -n "$REAL" ]; then
    STICK_ARGS="-device usb-host,bus=xhci.0,vendorid=0x303a,productid=0x$REAL,bootindex=1${RESETS:+,guest-resets-all=on}"
else
    STICK_ARGS="-drive if=none,id=stick,format=raw,readonly=on,file=out/stage1.img -device usb-storage,bus=xhci.0,drive=stick,bootindex=1"
fi
[ -n "$NOSTICK" ] && STICK_ARGS=
ss -ltn | grep -q "127.0.0.1:8000 " || (cd serve && setsid python3 -m http.server 8000 --bind 127.0.0.1 >../out/http.log 2>&1 &)
# NIC=none：虛擬機沒有自己的網卡，用來看 Stage-1 在沒網時的行為
if [ "$NIC" = none ]; then NET_ARGS="-nic none"
else NET_ARGS="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 -device ${NIC:-e1000e},netdev=n0"; fi
if [ -n "$BIOS" ]; then FW_ARGS=
else FW_ARGS="-drive if=pflash,format=raw,readonly=on,file=$R/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=out/vars.fd"; fi
exec $R/usr/bin/qemu-system-x86_64 -L $R/usr/share/qemu -L $R/usr/lib/ipxe/qemu -L $R/usr/share/seabios \
  -machine q35 -cpu max -smp 4 -m ${MEM:-4096} \
  $FW_ARGS \
  -device qemu-xhci,id=xhci \
  $STICK_ARGS \
  -drive if=none,id=hd,format=raw,file=$HD -device ide-hd,drive=hd,bus=ide.0 \
  $NET_ARGS \
  -display none -serial file:out/serial.log -monitor unix:out/qemu.mon,server,nowait "$@"
