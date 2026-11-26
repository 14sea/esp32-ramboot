#!/bin/sh
command -v getarg > /dev/null || . /lib/dracut-lib.sh
[ -e /ramroot.squashfs ] || return 0
ismounted "$NEWROOT" && return 0
mkdir -p /run/ramroot/ro /run/ramroot/rw
mount -t squashfs -o loop,ro /ramroot.squashfs /run/ramroot/ro || die "ramroot: cannot mount /ramroot.squashfs"
mount -t tmpfs -o mode=0755 tmpfs /run/ramroot/rw
mkdir -p /run/ramroot/rw/upper /run/ramroot/rw/work
mount -t overlay -o lowerdir=/run/ramroot/ro,upperdir=/run/ramroot/rw/upper,workdir=/run/ramroot/rw/work ramroot "$NEWROOT" \
    || die "ramroot: overlay mount failed"
# 每台機器的個性化文件（cloud-init 種子等），由最後一段 cpio 帶進來
[ -d /ramroot.d ] && cp -a /ramroot.d/. "$NEWROOT"/
# 內核參數 ramboot.console=1：第一個虛擬終端自動以 root 登入。給連不上這台機器網絡的情況用——
# 主機經藍牙鍵盤打命令（r <命令> 把輸出報回去）。默認不開：開了之後能碰到鍵盤的人就是 root
if getargbool 0 ramboot.console; then
    mkdir -p "$NEWROOT/etc/systemd/system/getty@tty1.service.d"
    printf '[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin root --noclear %%I $TERM\n' \
        > "$NEWROOT/etc/systemd/system/getty@tty1.service.d/autologin.conf"
fi
# 內核參數 ramboot.agent=1：開 ramboot-agent，主機用 kvm.py run / put 經板子跑命令、傳文件（不用打字、不用網絡）。
# 默認不開，理由同上：能控制板子的人就是 root
if getargbool 0 ramboot.agent && [ -f "$NEWROOT/etc/systemd/system/ramboot-agent.service" ]; then
    mkdir -p "$NEWROOT/etc/systemd/system/multi-user.target.wants"
    ln -sf ../ramboot-agent.service "$NEWROOT/etc/systemd/system/multi-user.target.wants/ramboot-agent.service"
fi
info "ramroot: root is squashfs + tmpfs overlay, all in RAM"
