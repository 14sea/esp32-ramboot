# TODO

Status of each task: **open**, **in work**, **done**, or **blocked**.

| No. | Task | Status |
|---|---|---|
| 1 | Test on a real computer | blocked: no target computer at this location |
| 1a | Test of the download from `cloud-images.ubuntu.com` in Stage 1 | open |
| 2 | Feedback channel from the target computer to the host computer | open |
| 3 | Change of the configuration without a new boot disk image | open |
| 4 | Operation with Secure Boot on | open |
| 5 | Installation of an operating system on the disk of the target computer | open |
| 6 | Wireless network and download of drivers on demand | blocked: refer to the task |

## 1. Test on a real computer

The boot sequence was tested only in QEMU with OVMF.
Real UEFI firmware can be different in these areas:

- The boot disk has a FAT16 partition. The size of 8 MiB is too small for FAT32.
- The device board is a composite USB device (keyboard, mouse, and disk).
- The UEFI firmware can have a short time limit for a read of the disk.
- `kexec` and the screen output after `kexec`.

Necessary: a target computer with a network cable.

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
