# TODO

Date of the last change: 2026-11-25.

## Open tasks

The sequence of the tasks is the sequence of their importance.

| No. | Task | Reason |
|---|---|---|
| 1 | Find why the USB network adapter stops when the host computer sends a key in the mode `net` | You cannot use the two functions at the same time |
| 2 | Install the device firmware with the correction for the second start of the mode `net`, and do a test | The author compiled the correction in `kvm_net.c` but did not do a test on a board |
| 3 | Read the Stage 2 files from a disk of the target computer | Each start downloads 386 MB. Through the board, the time is 12 minutes |
| 4 | Write the UEFI boot entries for an installed disk | A system that `kexec` started cannot write them |
| 5 | Update the device firmware through Bluetooth LE | Each firmware change makes it necessary to move the board to the host computer |
| 6 | Put the SSH key and the host name into the configuration of the device board | A change makes a new boot disk image necessary |
| 7 | Operate with Secure Boot on | The Stage 1 kernel has no signature |
| 8 | Download drivers on demand, for example for wireless network adapters | Stage 1 has only the drivers in its image |
| 9 | Control the power of the target computer (power button, reset, Wake-on-LAN) | A person must start the target computer |
| 10 | Do the tests that are not done | Refer to "Tests that are not done" |

### Notes for the open tasks

1. This occurred on a Lenovo ThinkCentre M91p (EHCI controller with a hub), not in QEMU.
   The kernel log of the target showed `usbnet` in `usb_clear_halt`. The keyboard also stopped.
   At this time, use the mode `hid+msc` to type and the mode `net` for the network.
3. Stage 1 can keep a copy of the three files on a FAT partition, for example the EFI system partition.
   At the next start, it reads the files from this partition and compares the SHA-256 values.
   Stage 1 must then have drivers for SATA and NVMe disks.
   At this time it has no disk drivers. This makes sure that it does not write to the disks.
4. The Lenovo firmware did not start the installed disk from the entry `ubuntu` or from the removable-media path.
   `install-to-disk.sh` adds a copy of the boot loader at the path of the Windows boot manager and a BIOS boot loader.
   It is not known which of the two the firmware uses.
7. A possible solution is a signed shim and a signed kernel from a Linux distribution.
   The signed kernels are large. The boot disk image has a maximum size of 12 MB.

### Tests that are not done

- A computer that has a BIOS but no UEFI firmware. QEMU with SeaBIOS is the only BIOS test.
- First pairing of two new boards.
- `install-to-disk.sh` on a computer with an NVMe disk, and in QEMU from start to end.

## Completed tasks

The author completed all of these tasks on 2026-11-25.

| Task | Result |
|---|---|
| Test on a real computer | Lenovo ThinkCentre M91p: start from the board, Ubuntu Server in RAM, installation on the disk |
| Download from `cloud-images.ubuntu.com` in Stage 1 | 386 MB in 704 seconds through the device board |
| Feedback channel from the target computer | Vendor SCSI commands on the boot disk. Commands `target` and `mtrace` |
| Configuration without a new boot disk image | Keys `base`, `cmdline`, and `netwait`. Command `cfg` |
| Start in BIOS mode | MBR partition table and syslinux. The Lenovo firmware did not start the GPT disk |
| USB network adapter for targets with no network | USB mode `net`. WPA2 and WPA2-Enterprise. Approximately 550 kB/s |
| Installation on the disk of the target computer | `tools/install-to-disk.sh` |
