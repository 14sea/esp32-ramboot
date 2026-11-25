#!/bin/sh
# Install Ubuntu Server from the RAM system to a disk of the target computer.
#
# Run this script as root in the Ubuntu Server system that esp32-ramboot started in RAM:
#     install-to-disk.sh /dev/sdX <user> <password> [hostname]     full installation
#     install-to-disk.sh --boot-only /dev/sdX                       install the boot loaders again on a disk
#                                                                   that this script installed before
#
# THE FULL INSTALLATION ERASES ALL DATA ON /dev/sdX.
#
# It copies the read-only Ubuntu Server root file system that is already in RAM (no installer image is necessary),
# then downloads only the kernel and the boot loaders (approximately 200 MB).
# Disk layout: GPT, BIOS boot partition (1 MiB), EFI system partition (FAT32, 512 MiB), ext4 root partition.
# The disk starts in UEFI mode and in BIOS mode:
#   - UEFI: shim and GRUB at \EFI\ubuntu, at the removable-media path \EFI\BOOT, and at \EFI\Microsoft\Boot.
#     Some firmware (for example Lenovo ThinkCentre M91p) starts only boot entries with the name
#     "Windows Boot Manager", and ignores other entries and the removable-media path on a hard disk.
#   - BIOS: GRUB in the MBR and the BIOS boot partition, for firmware that goes to BIOS mode when UEFI finds nothing.
# Progress goes to the screen and, if the board is there, to the host computer.
set -e
BOOT_ONLY=
[ "$1" = --boot-only ] && { BOOT_ONLY=1; shift; }
DISK=$1; USER_NAME=$2; USER_PW=$3; HOST_NAME=${4:-ubuntu}
RO=/run/ramroot/ro
T=/mnt/target
# The name of the board's disk can change when the board changes its USB mode: find it for each message
board() {
    for b in /sys/block/sd*; do
        case "$(cat "$b/device/model" 2>/dev/null)" in *"S3 Boot Disk"*) echo /dev/${b##*/}; return 0 ;; esac
    done
    return 1
}
say() {
    echo "[install] $*"
    [ -x /usr/local/sbin/s1tool ] && b=$(board) && /usr/local/sbin/s1tool msg "$b" "os install: $*" 2>/dev/null
    return 0
}
die() { say "FAILED: $*"; exit 1; }
# With "set -e" a failed command stops the script without a message: report the step in which this occurred
STEP=start
trap '[ $? = 0 ] || say "FAILED in step: $STEP"' EXIT
BOARD=$(board) || BOARD=

[ "$(id -u)" = 0 ] || die "run as root"
[ -b "$DISK" ] || die "usage: $0 [--boot-only] /dev/sdX user password [hostname]"
[ "$DISK" != "$BOARD" ] || die "$DISK is the boot disk of the board"
grep -q "^$DISK" /proc/mounts && die "$DISK is mounted"
case "$DISK" in *nvme*|*mmcblk*) P=p ;; *) P= ;; esac

mount_chroot() {
    # rslave: an unmount in the target must not go back to the RAM system
    for d in dev proc sys run; do mount --rbind /$d $T/$d; mount --make-rslave $T/$d; done
}
wait_dns() {    # the network can come later (the board changes its USB mode)
    n=0
    until chroot $T getent hosts archive.ubuntu.com >/dev/null 2>&1; do
        n=$((n + 1)); [ $n -lt 120 ] || die "no network (DNS) after 10 minutes"; sleep 5
    done
}

if [ -z "$BOOT_ONLY" ]; then
    [ -n "$USER_NAME" ] && [ -n "$USER_PW" ] || die "usage: $0 /dev/sdX user password [hostname]"
    [ -d $RO/usr ] || die "no read-only root at $RO (this is not the RAM system)"
    STEP=partition
    say "start: disk $DISK ($(lsblk -dn -o SIZE,MODEL "$DISK" | tr -s ' '))"
    wipefs -aq "$DISK"
    printf 'label: gpt\nsize=1MiB, type=21686148-6449-6E6F-744E-656564454649, name=bios\nsize=512MiB, type=uefi, name=EFI\ntype=linux, name=root\n' \
        | sfdisk -q --wipe always --wipe-partitions always "$DISK"
    udevadm settle
    command -v mkfs.vfat >/dev/null || { say "install dosfstools"; apt-get -qq update && apt-get -qq install -y dosfstools; }
    mkfs.vfat -F 32 -n EFI "${DISK}${P}2" >/dev/null
    mkfs.ext4 -q -F -L root "${DISK}${P}3"
    mkdir -p $T && mount "${DISK}${P}3" $T
    mkdir -p $T/boot/efi

    STEP=copy
    say "copy the system ($(du -sm $RO 2>/dev/null | cut -f1) MB)"
    cp -a $RO/. $T/
    mount "${DISK}${P}2" $T/boot/efi

    STEP=configure
    say "configure"
    cat > $T/etc/fstab <<EOF
UUID=$(blkid -s UUID -o value "${DISK}${P}3") /         ext4 defaults 0 1
UUID=$(blkid -s UUID -o value "${DISK}${P}2") /boot/efi vfat umask=0077 0 1
EOF
    echo "$HOST_NAME" > $T/etc/hostname
    sed -i '/^127\.0\.1\.1/d' $T/etc/hosts
    echo "127.0.1.1 $HOST_NAME" >> $T/etc/hosts
    # No cloud datasource on a real disk: do the work of cloud-init here and switch it off
    touch $T/etc/cloud/cloud-init.disabled
    mkdir -p $T/etc/netplan
    cat > $T/etc/netplan/01-dhcp.yaml <<EOF
network:
  version: 2
  ethernets:
    all-en:
      match: {name: "en*"}
      dhcp4: true
      optional: true
    all-eth:
      match: {name: "eth*"}
      dhcp4: true
      optional: true
EOF
    chmod 600 $T/etc/netplan/01-dhcp.yaml
    mount_chroot
    chroot $T sh -e <<EOF
useradd -m -s /bin/bash -G sudo "$USER_NAME"
echo "$USER_NAME:$USER_PW" | chpasswd
ssh-keygen -A >/dev/null
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config
rm -f /etc/ssh/sshd_config.d/60-cloudimg-settings.conf
EOF
else
    # --boot-only: the layout can be the old one (EFI = 1, root = 2) or the current one (bios = 1, EFI = 2, root = 3)
    STEP=mount
    say "start (boot loaders only): disk $DISK"
    ESP=$(lsblk -lnpo NAME,PARTTYPE "$DISK" | awk 'tolower($2)=="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"{print $1; exit}')
    ROOT=$(lsblk -lnpo NAME,FSTYPE "$DISK" | awk '$2=="ext4"{print $1; exit}')
    [ -n "$ESP" ] && [ -n "$ROOT" ] || die "no EFI system partition or no ext4 root on $DISK"
    mkdir -p $T && mount "$ROOT" $T && mount "$ESP" $T/boot/efi
    mount_chroot
    if ! lsblk -lno PARTTYPE "$DISK" | grep -qi 21686148-6449-6e6f-744e-656564454649; then
        # The first MiB before the first partition is free: put the BIOS boot partition there
        say "add a BIOS boot partition"
        printf 'start=34, size=2014, type=21686148-6449-6E6F-744E-656564454649, name=bios\n' | sfdisk -q --append --no-reread --force "$DISK"
        partx -u "$DISK" 2>/dev/null || partprobe "$DISK" 2>/dev/null || true
        udevadm settle
    fi
fi

STEP=download
say "download the kernel and the boot loaders"
wait_dns
chroot $T sh -e <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq install -y --no-install-recommends linux-image-virtual grub-efi-amd64 grub-efi-amd64-signed shim-signed grub-pc-bin
EOF

STEP=bootloader
say "install the boot loaders"
chroot $T sh -e <<EOF
sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT=""/' /etc/default/grub
rm -f /etc/default/grub.d/50-cloudimg-settings.cfg
# UEFI. --removable also writes EFI/BOOT/BOOTX64.EFI
grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable --no-nvram >/dev/null 2>&1
grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=ubuntu --no-nvram >/dev/null 2>&1
# A copy at the path of the Windows boot manager, for firmware that looks only there
mkdir -p /boot/efi/EFI/Microsoft/Boot
cp /boot/efi/EFI/ubuntu/shimx64.efi /boot/efi/EFI/Microsoft/Boot/bootmgfw.efi
cp /boot/efi/EFI/ubuntu/grubx64.efi /boot/efi/EFI/ubuntu/mmx64.efi /boot/efi/EFI/Microsoft/Boot/ 2>/dev/null || true
# BIOS
grub-install --target=i386-pc "$DISK" >/dev/null 2>&1
update-grub >/dev/null 2>&1
EOF
# BIOS mode on a GPT disk: some firmware starts the disk only if the protective MBR entry has the active flag
printf '\200' | dd of="$DISK" bs=1 seek=446 conv=notrunc status=none

STEP=nvram
# Boot entries in the firmware (only possible if the RAM system started in UEFI mode). Failure here is not fatal
if [ -d /sys/firmware/efi ]; then
    ESPDEV=$(findmnt -no SOURCE $T/boot/efi); ESPNUM=$(cat /sys/class/block/${ESPDEV##*/}/partition)
    chroot $T sh <<EOF || true
for n in \$(efibootmgr | sed -n 's/^Boot\([0-9A-F]\{4\}\)[* ] \(ubuntu\|Windows Boot Manager\)\$/\1/p'); do efibootmgr -q -b \$n -B; done
efibootmgr -q -c -d "$DISK" -p $ESPNUM -L ubuntu -l '\EFI\ubuntu\shimx64.efi'
efibootmgr -q -c -d "$DISK" -p $ESPNUM -L "Windows Boot Manager" -l '\EFI\ubuntu\shimx64.efi'
EOF
    say "boot entries: $(chroot $T efibootmgr 2>/dev/null | grep -c '^Boot[0-9A-F]\{4\}') in the firmware, order $(chroot $T efibootmgr 2>/dev/null | sed -n 's/^BootOrder: //p')"
fi

STEP=check
K=$(ls $T/boot/vmlinuz-* 2>/dev/null | tail -1)
[ -n "$K" ] && [ -s $T/boot/grub/grub.cfg ] && [ -s $T/boot/efi/EFI/BOOT/BOOTX64.EFI ] || die "kernel or boot loader is missing"
ls $T/boot/initrd.img-* >/dev/null 2>&1 || die "no initrd was made"
[ -d $T/boot/grub/i386-pc ] || die "no BIOS boot loader"

STEP=unmount
sync
umount -R $T 2>/dev/null || umount -R -l $T
sync
STEP=done
say "done: kernel ${K##*/vmlinuz-}. Remove the boot disk of the board and start the computer again."
