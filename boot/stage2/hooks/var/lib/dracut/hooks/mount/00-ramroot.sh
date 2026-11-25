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
info "ramroot: root is squashfs + tmpfs overlay, all in RAM"
