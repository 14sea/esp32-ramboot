#!/bin/sh
# Install Ubuntu Server from the RAM system to a disk of the target computer.
#
# Run this script as root in the Ubuntu Server system that esp32-ramboot started in RAM:
#     install-to-disk.sh /dev/sdX <user> <password> <hostname>
#
# THE SCRIPT ERASES ALL DATA ON /dev/sdX.
#
# It copies the read-only Ubuntu Server root file system that is already in RAM (no installer image is necessary),
# then downloads only the kernel and the boot loader (approximately 150 MB).
# The disk gets a GPT with an EFI system partition (FAT32, 512 MiB) and an ext4 root partition. UEFI boot only.
# Progress goes to the screen and, if the board is there, to the host computer (ramboot-report).
set -e
DISK=$1; USER_NAME=$2; USER_PW=$3; HOST_NAME=${4:-ubuntu}
RO=/run/ramroot/ro
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
BOARD=$(board) || BOARD=

[ "$(id -u)" = 0 ] || die "run as root"
[ -b "$DISK" ] && [ -n "$USER_NAME" ] && [ -n "$USER_PW" ] || die "usage: $0 /dev/sdX user password [hostname]"
[ "$DISK" != "$BOARD" ] || die "$DISK is the boot disk of the board"
[ -d $RO/usr ] || die "no read-only root at $RO (this is not the RAM system)"
grep -q "^$DISK" /proc/mounts && die "$DISK is mounted"
case "$DISK" in *nvme*|*mmcblk*) P=p ;; *) P= ;; esac

say "start: disk $DISK ($(lsblk -dn -o SIZE,MODEL "$DISK" | tr -s ' '))"
wipefs -aq "$DISK"
printf 'label: gpt\nsize=512MiB, type=uefi, name=EFI\ntype=linux, name=root\n' | sfdisk -q --wipe always --wipe-partitions always "$DISK"
udevadm settle
command -v mkfs.vfat >/dev/null || { say "install dosfstools"; apt-get -qq update && apt-get -qq install -y dosfstools; }
mkfs.vfat -F 32 -n EFI "${DISK}${P}1" >/dev/null
mkfs.ext4 -q -F -L root "${DISK}${P}2"
T=/mnt/target
mkdir -p $T && mount "${DISK}${P}2" $T
mkdir -p $T/boot/efi

say "copy the system ($(du -sm $RO 2>/dev/null | cut -f1) MB)"
cp -a $RO/. $T/
mount "${DISK}${P}1" $T/boot/efi

say "configure"
cat > $T/etc/fstab <<EOF
UUID=$(blkid -s UUID -o value "${DISK}${P}2") /         ext4 defaults 0 1
UUID=$(blkid -s UUID -o value "${DISK}${P}1") /boot/efi vfat umask=0077 0 1
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
# /run comes from the RAM system: /etc/resolv.conf in the target points into it, so DNS works in the chroot
for d in dev proc sys run; do mount --rbind /$d $T/$d; done

chroot $T sh -e <<EOF
useradd -m -s /bin/bash -G sudo "$USER_NAME"
echo "$USER_NAME:$USER_PW" | chpasswd
ssh-keygen -A >/dev/null
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config
rm -f /etc/ssh/sshd_config.d/60-cloudimg-settings.conf
EOF

say "download the kernel and the boot loader"
n=0; until chroot $T getent hosts archive.ubuntu.com >/dev/null 2>&1; do      # the network can come later (USB mode change)
    n=$((n + 1)); [ $n -lt 120 ] || die "no network (DNS) after 10 minutes"; sleep 5
done
chroot $T sh -e <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq install -y --no-install-recommends linux-image-virtual grub-efi-amd64 grub-efi-amd64-signed shim-signed
EOF

say "install the boot loader"
chroot $T sh -e <<'EOF'
sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT=""/' /etc/default/grub
rm -f /etc/default/grub.d/50-cloudimg-settings.cfg
# --removable also writes EFI/BOOT/BOOTX64.EFI: the disk starts when the firmware has no boot entry for it
grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable --no-nvram >/dev/null 2>&1
grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=ubuntu >/dev/null 2>&1 || true
update-grub >/dev/null 2>&1
EOF
K=$(ls $T/boot/vmlinuz-* 2>/dev/null | tail -1)
[ -n "$K" ] && [ -s $T/boot/grub/grub.cfg ] && [ -s $T/boot/efi/EFI/BOOT/BOOTX64.EFI ] || die "kernel or boot loader is missing"
ls $T/boot/initrd.img-* >/dev/null 2>&1 || die "no initrd was made"

sync
umount -R $T
say "done: kernel ${K##*/vmlinuz-}, user $USER_NAME, hostname $HOST_NAME. Remove the boot disk and start the computer again."
