# TODO

Status of each task: **open**, **in work**, **done**, or **blocked**.

| No. | Task | Status |
|---|---|---|
| 1 | Test on a real computer | done on 2026-11-25 with a Lenovo ThinkCentre M91p. Refer to the task |
| 1a | Test of the download from `cloud-images.ubuntu.com` in Stage 1 | done on 2026-11-25: 386 MB in 704 s through the device board |
| 2 | Feedback channel from the target computer to the host computer | done on 2026-11-25 |
| 3 | Change of the configuration without a new boot disk image | in work: download location, kernel parameters, and wait time are done. The SSH key is open |
| 4 | Operation with Secure Boot on | open |
| 5 | Installation of an operating system on the disk of the target computer | in work: `tools/install-to-disk.sh` installed Ubuntu Server on the ThinkCentre |
| 6 | Wireless network and download of drivers on demand | in work: the USB mode `net` is done. Download of drivers on demand is open |
| 7 | Problems of the USB mode `net` | open |
| 8 | Stage 2 files from a disk of the target computer | open |

## 1. Test on a real computer

The boot sequence was tested only in QEMU with OVMF.
Real UEFI firmware can be different in these areas:

- The boot disk has an MBR partition table and a FAT16 partition. The size of 8 MiB is too small for FAT32.
- The device board is a composite USB device (keyboard, mouse, and disk).
- The UEFI firmware can have a short time limit for a read of the disk.
- `kexec` and the screen output after `kexec`.

Results with a Lenovo ThinkCentre M91p (UEFI, no network cable, no screen at the start):

- The firmware did not start a boot disk with GPT and a FAT16 partition. It read only the partition table and
  the file system data. It started the same files from a disk with an MBR partition table.
- The firmware started the composite USB device (keyboard, mouse, and disk).
- `kexec` was correct. Ubuntu Server operated in RAM and reported the hardware through the board.
- The firmware did not start the installed disk from the entry `ubuntu` or from the removable-media path.
  It started the disk after the script added a copy of the boot loader at the path of the Windows boot manager
  and a BIOS boot loader. It is not known which of the two the firmware uses.
- A system that `kexec` started cannot write UEFI boot entries (`efibootmgr` showed 0 entries).

Open: a test on a computer with BIOS only. QEMU with SeaBIOS is the only BIOS test.

## 2. Feedback channel

The host computer gets no data from the screen of the target computer.
Stage 1 must send its progress to the device board with a vendor SCSI command on the boot disk.
The device board must send this data to the host computer as events.
Ubuntu Server must send its IP address in the same way.

## 3. Configuration without a new image

The download location and the SSH key are in the boot disk image.
A change makes a new image and a new installation necessary.
The device board must keep a small configuration that the host computer can write through Bluetooth LE.
Stage 1 must read this configuration from the device board.

## 4. Secure Boot

The Stage 1 kernel has no signature. The UEFI firmware does not start it when Secure Boot is on.
A possible solution is a signed shim and a signed kernel from a Linux distribution.
The signed kernels are large. The boot disk image has a maximum size of 12 MB.

## 5. Installation on the disk

The project stops when Ubuntu Server operates in RAM.
A procedure or a script to write an operating system to the disk is necessary.
This task erases data. The selection of the correct disk must be safe.

## 6. Wireless network and drivers on demand

Stage 1 has no drivers for wireless network adapters.
The ESP32S3CAM project has a USB network adapter mode for the device board.
With this mode, Stage 1 can download only the drivers that the target computer needs.
The wireless link of that mode is not stable. This problem must be solved first.

## 7. Problems of the USB mode `net`

- When the host computer sends a key in the mode `net`, the USB network adapter on the target stops.
  The kernel log of the target shows `usbnet` in `usb_clear_halt`. The keyboard also stops.
  This occurred on the ThinkCentre (EHCI controller behind a hub), not in QEMU. The cause is not known.
  At this time, use the mode `hid+msc` to type and the mode `net` for the network.
- The second start of the mode `net` without a restart of the board failed with a timeout.
  A correction is in `kvm_net.c`. It was compiled but not tested on a board.
- The rate through the board is approximately 550 kB/s.

## 8. Stage 2 files from a disk of the target computer

Each start downloads 386 MB. Through the device board, the time is approximately 12 minutes.
Stage 1 can keep a copy of the three files on a FAT partition of the target computer, for example the EFI
system partition (512 MiB). At the next start, Stage 1 reads the files from this partition and compares the
SHA-256 values. Stage 1 must then have drivers for SATA and NVMe disks. At this time it has no disk drivers,
which makes sure that it does not write to the disks.
