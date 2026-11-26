#!/bin/sh
# Install Ubuntu Server from the RAM system to a disk of the target computer.
#
# Run this script as root in the Ubuntu Server system that esp32-ramboot started in RAM:
#     install-to-disk.sh [options] /dev/sdX <user> <password> [hostname]     full installation
#     install-to-disk.sh --boot-only /dev/sdX                                 install the boot loaders again on a disk
#                                                                             that this script installed before
# Options:
#     --firmware auto|all|none   hardware firmware (default auto; with --boot-only the default is none).
#                                auto: install only the firmware packages for the files that the RAM system could not
#                                load (dmesg), for example radeon/* -> AMD graphics.
#                                all: all firmware (approximately 700 MB). none: no firmware
#     --desktop                  also install the GNOME desktop (ubuntu-desktop-minimal, approximately 470 MB),
#                                without the snap packages (firefox, thunderbird): see the note at the desktop step
#     --keep-boot-disk           do not tell the board to hide its boot disk at the next restart (see the last step)
#
# THE FULL INSTALLATION ERASES ALL DATA ON /dev/sdX.
#
# The installed system keeps these parts of the RAM system:
#   - the SSH public key of the boot image (boot/keys/id_ed25519.pub) in ~/.ssh/authorized_keys of <user>
#   - the feedback tools (s1tool, ramboot-report, r) and ramboot-report.service: after each start, the host
#     receives the IP address and the hardware data of the computer (kvm.py target)
#   - ramboot-agent (kvm.py run / put), enabled only if the RAM system started with ramboot.agent=1
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
# Do all mounts in a private mount namespace. Without this, the mounts of the target go into the namespaces of
# system services (systemd-networkd, systemd-logind, ...) and stay there: the disk is then busy for the next run.
# The namespace goes away when the script stops, and all its mounts with it.
if [ -z "$RAMBOOT_NS" ]; then
    command -v unshare >/dev/null && exec env RAMBOOT_NS=1 unshare -m --propagation private sh "$0" "$@"
fi
BOOT_ONLY= FIRMWARE= DESKTOP= HIDE=1
while :; do
    case "$1" in
        --boot-only) BOOT_ONLY=1; shift ;;
        --firmware) FIRMWARE=$2; shift 2 ;;
        --desktop) DESKTOP=1; shift ;;
        --keep-boot-disk) HIDE=; shift ;;
        *) break ;;
    esac
done
# --boot-only installs only the boot loaders again: no firmware, if the operator does not ask for it
if [ -z "$FIRMWARE" ]; then
    if [ -n "$BOOT_ONLY" ]; then FIRMWARE=none; else FIRMWARE=auto; fi
fi
case "$FIRMWARE" in auto|all|none) ;; *) echo "--firmware: auto, all or none" >&2; exit 1 ;; esac
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
# A run that stopped before the end can leave the target mounted
if grep -q " $T " /proc/mounts; then umount -R $T 2>/dev/null || umount -R -l $T; sleep 1; fi
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
    # The SSH public keys of the boot image: cloud-init gave them to the RAM system from its seed
    KEYS=$(grep -hoE '(ssh-(ed25519|rsa)|ecdsa-sha2-nistp[0-9]+|sk-[a-z0-9@.-]+) [A-Za-z0-9+/=]+( [^"]*)?' \
               /var/lib/cloud/seed/nocloud/user-data 2>/dev/null | sort -u)
    if [ -n "$KEYS" ]; then
        H=$T/home/$USER_NAME/.ssh
        mkdir -p "$H" && printf '%s\n' "$KEYS" > "$H/authorized_keys"
        chmod 700 "$H" && chmod 600 "$H/authorized_keys"
        chroot $T chown -R "$USER_NAME:$USER_NAME" "/home/$USER_NAME/.ssh"
        say "SSH keys: $(printf '%s\n' "$KEYS" | wc -l)"
    else
        say "no SSH key in the seed: only password login"
    fi
    # The feedback tools. They are in the overlay of the RAM system (from the seed), not in $RO
    for f in /usr/local/sbin/s1tool /usr/local/sbin/ramboot-report /usr/local/sbin/ramboot-agent /usr/local/bin/r \
             /etc/systemd/system/ramboot-agent.service; do
        if [ -e "$f" ]; then install -D -m "$(stat -c %a "$f")" "$f" "$T$f"; fi
    done
    # Report to the host after each start. The board can hide its boot disk for some time after a restart
    # (see the last step): try again for 5 minutes
    cat > $T/etc/systemd/system/ramboot-report.service <<'EOF'
[Unit]
Description=esp32-ramboot: report this computer to the host through the boot disk board
Wants=network-online.target
After=network-online.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for i in $(seq 60); do /usr/local/sbin/ramboot-report 2>/dev/null && exit 0; sleep 5; done; exit 0'

[Install]
WantedBy=multi-user.target
EOF
    mkdir -p $T/etc/systemd/system/multi-user.target.wants
    if [ -x $T/usr/local/sbin/ramboot-report ]; then
        ln -sf ../ramboot-report.service $T/etc/systemd/system/multi-user.target.wants/ramboot-report.service
    fi
    if [ -f $T/etc/systemd/system/ramboot-agent.service ] && grep -qw 'ramboot.agent=1' /proc/cmdline; then
        ln -sf ../ramboot-agent.service $T/etc/systemd/system/multi-user.target.wants/ramboot-agent.service
        say "ramboot-agent: enabled (the RAM system started with ramboot.agent=1)"
    fi
else
    STEP=mount
    say "start (boot loaders only): disk $DISK"
    ESP=$(lsblk -lnpo NAME,PARTTYPE "$DISK" | awk 'tolower($2)=="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"{print $1; exit}')
    ROOT=$(lsblk -lnpo NAME,FSTYPE "$DISK" | awk '$2=="ext4"{print $1; exit}')
    [ -n "$ESP" ] && [ -n "$ROOT" ] || die "no EFI system partition or no ext4 root on $DISK"
    mkdir -p $T && mount "$ROOT" $T && mount "$ESP" $T/boot/efi
    mount_chroot
fi
# GRUB for BIOS mode needs a BIOS boot partition on a GPT disk
BIOS=
lsblk -lno PARTTYPE "$DISK" | grep -qi 21686148-6449-6e6f-744e-656564454649 && BIOS=1
[ -n "$BIOS" ] || say "no BIOS boot partition: the disk will start in UEFI mode only"

STEP=download
say "download the kernel and the boot loaders"
wait_dns
chroot $T sh -e <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq install -y --no-install-recommends linux-image-virtual grub-efi-amd64 grub-efi-amd64-signed shim-signed grub-pc-bin
EOF

# Hardware firmware. linux-image-virtual has the same drivers as linux-image-generic, but no firmware files
# (linux-image-generic needs all of linux-firmware, approximately 700 MB). Example: on the ThinkCentre M91p the
# AMD Radeon HD 6450 did not start without radeon/CAICOS_*.bin.
# auto: find the files that the RAM system (same kernel) could not load, and install the vendor packages for them.
# Ubuntu 26.04 divides linux-firmware into vendor packages (linux-firmware-amd-graphics, ...).
fw_pkg() {      # firmware file -> package
    case "$1" in
        radeon/*|amdgpu/*) echo linux-firmware-amd-graphics ;;
        amd/*|amd-ucode/*|amdtee/*) echo linux-firmware-amd-misc ;;
        i915/*|xe/*) echo linux-firmware-intel-graphics ;;
        iwlwifi-*|intel/ibt-*|intel/iwlwifi/*) echo linux-firmware-intel-wireless ;;
        intel/*) echo linux-firmware-intel-misc ;;
        brcm/*|cypress/*) echo linux-firmware-broadcom-wireless ;;
        rtl_nic/*|rtlwifi/*|rtw88/*|rtw89/*|rtl_bt/*) echo linux-firmware-realtek ;;
        mediatek/*|mt7*) echo linux-firmware-mediatek ;;
        nvidia/*) echo linux-firmware-nvidia-graphics ;;
        ath9k*|ath10k/*|ath11k/*|ath12k/*|qca/*) echo linux-firmware-qualcomm-wireless ;;
        mrvl/*|libertas/*) echo linux-firmware-marvell-wireless ;;
        *) echo linux-firmware-misc ;;
    esac
}
STEP=firmware
FW_PKGS=
if [ "$FIRMWARE" = all ]; then
    FW_PKGS=linux-firmware
elif [ "$FIRMWARE" = auto ]; then
    MISSING=$( { dmesg 2>/dev/null || journalctl -k -b 2>/dev/null; } |
               sed -n 's/.*Direct firmware load for \([^ ]*\) failed.*/\1/p; s/.*firmware: failed to load \([^ ]*\).*/\1/p' | sort -u)
    for f in $MISSING; do FW_PKGS="$FW_PKGS $(fw_pkg "$f")"; done
    FW_PKGS=$(printf '%s\n' $FW_PKGS | sort -u | tr '\n' ' ')
    [ -z "$MISSING" ] || say "missing firmware: $(printf '%s\n' $MISSING | wc -l) files ($(printf '%s ' $MISSING | cut -c1-60))"
fi
if [ -n "${FW_PKGS% }" ]; then
    say "install firmware: $FW_PKGS"
    # One package at a time: a package that is not available must not stop the others.
    # Not fatal: the system starts without the firmware (only the hardware does not work)
    FW_OK=0
    for p in $FW_PKGS; do
        if chroot $T sh -c "DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends $p"; then
            FW_OK=$((FW_OK + 1))
        else
            say "firmware package $p: installation failed (continue without it)"
        fi
    done
    # The kernel package made the initramfs before the firmware was there. A driver in the initramfs (for example
    # radeon for the early display) needs its firmware in the initramfs: make it again. A firmware package does
    # not always do this itself
    if [ "$FW_OK" -gt 0 ]; then
        chroot $T update-initramfs -u -k all >/dev/null 2>&1 || say "update-initramfs failed (continue)"
        say "firmware: $FW_OK packages, initramfs updated"
    fi
fi

# GNOME desktop. Ubuntu's firefox and thunderbird debs are transitional packages: their preinst downloads a large
# snap while dpkg waits. Through the board (approximately 550 kB/s) this took more than 10 minutes, and an
# interruption left dpkg with firefox in the state iHR. In ubuntu-desktop-minimal, firefox is only "Recommends":
# a pin with priority -1 keeps these packages out and does not break the installation.
if [ -n "$DESKTOP" ]; then
    STEP=desktop
    say "install the GNOME desktop (ubuntu-desktop-minimal, approximately 470 MB)"
    cat > $T/etc/apt/preferences.d/ramboot-no-snap-transitional <<'EOF'
# Written by esp32-ramboot install-to-disk.sh: these debs only download a snap in their preinst.
# Install the browser later, for example "snap install firefox", or remove this file.
Package: firefox thunderbird chromium-browser
Pin: release *
Pin-Priority: -1
EOF
    chroot $T sh -e <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get -qq install -y -o DPkg::Lock::Timeout=600 ubuntu-desktop-minimal
EOF
    # A desktop computer gets its network from NetworkManager; keep the netplan file for the server interfaces
    say "desktop installed"
fi

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
[ -z "$BIOS" ] || grub-install --target=i386-pc "$DISK" >/dev/null 2>&1
update-grub >/dev/null 2>&1
EOF
# BIOS mode on a GPT disk: some firmware starts the disk only if the protective MBR entry has the active flag
[ -z "$BIOS" ] || printf '\200' | dd of="$DISK" bs=1 seek=446 conv=notrunc status=none

STEP=nvram
# Boot entries in the firmware (only possible if the RAM system started in UEFI mode). Failure here is not fatal
if [ -d /sys/firmware/efi ]; then
    ESPDEV=$(findmnt -no SOURCE $T/boot/efi); ESPNUM=$(cat /sys/class/block/${ESPDEV##*/}/partition)
    chroot $T sh <<EOF || true
for n in \$(efibootmgr | sed -n 's/^Boot\([0-9A-F]\{4\}\)[* ] \(ubuntu\|Windows Boot Manager\)\([[:space:]].*\)\{0,1\}\$/\1/p'); do efibootmgr -q -b \$n -B; done
efibootmgr -q -c -d "$DISK" -p $ESPNUM -L ubuntu -l '\EFI\ubuntu\shimx64.efi'
efibootmgr -q -c -d "$DISK" -p $ESPNUM -L "Windows Boot Manager" -l '\EFI\ubuntu\shimx64.efi'
EOF
    say "boot entries: $(chroot $T efibootmgr 2>/dev/null | grep -c '^Boot[0-9A-F]\{4\}') in the firmware, order $(chroot $T efibootmgr 2>/dev/null | sed -n 's/^BootOrder: //p')"
fi

STEP=check
K=$(ls $T/boot/vmlinuz-* 2>/dev/null | tail -1)
[ -n "$K" ] && [ -s $T/boot/grub/grub.cfg ] && [ -s $T/boot/efi/EFI/BOOT/BOOTX64.EFI ] || die "kernel or boot loader is missing"
ls $T/boot/initrd.img-* >/dev/null 2>&1 || die "no initrd was made"
[ -z "$BIOS" ] || [ -d $T/boot/grub/i386-pc ] || die "no BIOS boot loader"

STEP=unmount
sync
umount -R $T 2>/dev/null || umount -R -l $T
sync
# Firmware that gives priority to USB disks would start Stage 1 again at the next restart. Tell the board to hide
# its boot disk at the next restart (one time; the saved boot mode of the board does not change), and to show it
# again after 180 seconds: then the installed system can use the feedback tools.
# Board firmware older than 0.12 does not know this command: then remove the board or use "kvm.py mode hid".
STEP=done
MSG="Remove the boot disk of the board and start the computer again."
if [ -n "$HIDE" ] && [ -x /usr/local/sbin/s1tool ] && b=$(board) && /usr/local/sbin/s1tool next "$b" "hid 180" 2>/dev/null; then
    MSG="Start the computer again: the board hides its boot disk at the restart (for 180 seconds)."
fi
say "done: kernel ${K##*/vmlinuz-}. $MSG"
