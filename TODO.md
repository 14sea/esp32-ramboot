# TODO

Date of the last change: 2026-11-26.

## Open tasks

The sequence of the tasks is the sequence of their importance.

| No. | Task | Reason |
|---|---|---|
| 1 | Find why the USB network adapter stops when the host computer sends a key in the mode `net` | You cannot use the two functions at the same time |
| 2 | Find why inbound SSH times out before banner exchange in the mode `net` | Outbound downloads work, but SSH connections to the target over `eduroam` time out before authentication |
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
   With `ramboot.agent=1`, `kvm.py run` does not use the keyboard: use it in the mode `net`.
2. Tested on the ThinkCentre over `eduroam` (`10.95.0.0/16`): TCP connections to port 22 completed the
   handshake, but the client received no SSH banner and `sshd` logged `Timeout before authentication`.
   Closed ports returned `Connection refused`.
   Analysis on 2026-11-26 (not tested):
   - The board sends the Ethernet frames between USB and WiFi with no check of their contents (`kvm_net.c`).
     The handshake and the `RST` of closed ports go through. Thus the addresses and the path are correct.
   - The first data segments in the two directions (the two SSH banners) did not arrive.
     These segments are smaller than 100 bytes. Thus the frame size and the MTU are not a probable cause.
   - A probable cause is a firewall of the network that identifies the protocol from the first data
     ("application identification") and blocks inbound SSH to wireless clients.
   - Test: on the target, start a listener that sends a line of plain text (for example on port 8080),
     and a listener that sends an SSH banner (for example `SSH-2.0-test` on port 2222).
     Connect from a computer on the same network. If the plain text arrives and the SSH banner does not,
     the network is the cause. On 2026-11-26 the host computer was not on `eduroam`.
   - Workarounds: `kvm.py run` and `kvm.py put` (no network necessary), or an outbound connection
     from the target, for example a reverse SSH tunnel.
3. Stage 1 can keep a copy of the three files on a FAT partition, for example the EFI system partition.
   At the next start, it reads the files from this partition and compares the SHA-256 values.
   Stage 1 must then have drivers for SATA and NVMe disks.
   At this time it has no disk drivers. This makes sure that it does not write to the disks.
4. The Lenovo firmware did not start the installed disk from the entry `ubuntu` or from the removable-media path.
   `install-to-disk.sh` adds a copy of the boot loader at the path of the Windows boot manager and a BIOS boot loader.
   It is not known which of the two the firmware uses.
   The board can now hide its boot disk at the restart after the installation (`mode next`).
7. A possible solution is a signed shim and a signed kernel from a Linux distribution.
   The signed kernels are large. The boot disk image has a maximum size of 12 MB.

### Tests that are not done

- A computer that has a BIOS but no UEFI firmware. QEMU with SeaBIOS is the only BIOS test.
- First pairing of two new boards.
- `install-to-disk.sh` on a computer with an NVMe disk, and in QEMU from start to end.
- `install-to-disk.sh` with the changes of 2026-11-26 on a real computer: SSH key, feedback tools and
  `ramboot-report.service`, `--firmware auto` (the table of firmware files and packages has no test),
  `--desktop`, and the hidden boot disk at the restart.
- `ramboot-agent` with the real `s1tool` on a target computer. The tests on 2026-11-26 used
  `tools/usbfs_target.py` in the place of `s1tool` in WSL2 (no `root` access), and the board firmware 0.12.
- `mode next` at a real restart of a target computer. The tests used USB/IP detach and attach in WSL2,
  which make USB bus resets.

## Completed tasks

| Date | Task | Result |
|---|---|---|
| 2026-11-25 | Test on a real computer | Lenovo ThinkCentre M91p: start from the board, Ubuntu Server in RAM, installation on the disk |
| 2026-11-25 | Download from `cloud-images.ubuntu.com` in Stage 1 | 386 MB in 704 seconds through the device board |
| 2026-11-25 | Feedback channel from the target computer | Vendor SCSI commands on the boot disk. Commands `target` and `mtrace` |
| 2026-11-25 | Configuration without a new boot disk image | Keys `base`, `cmdline`, and `netwait`. Command `cfg` |
| 2026-11-25 | Start in BIOS mode | MBR partition table and syslinux. The Lenovo firmware did not start the GPT disk |
| 2026-11-25 | USB network adapter for targets with no network | USB mode `net`. WPA2 and WPA2-Enterprise. Approximately 550 kB/s |
| 2026-11-25 | Installation on the disk of the target computer | `tools/install-to-disk.sh` |
| 2026-11-26 | Second start of the mode `net` | Firmware 0.12. Three starts with no restart of the board |
| 2026-11-26 | `type` keeps the spaces at the start of the text | Firmware 0.12. Only `type`: the other commands skip all spaces |
| 2026-11-26 | Hide the boot disk at the next restart of the target | `kvm.py mode next <mode> [seconds]`, vendor SCSI command `NEXT` (`s1tool next`). `install-to-disk.sh` uses `hid 180` |
| 2026-11-26 | Command and file channel from the host to the target | Queue `tq` on the board, vendor SCSI command `TGET` (`s1tool get`), `ramboot-agent`, `kvm.py run` and `put`. Approximately 5 kB/s |
| 2026-11-26 | WiFi data in the NVS | `kvm.py wifi load`, `wifi set`, `wifi forget`. The data in the NVS has priority over `wifi_secrets.h` |
| 2026-11-26 | `install-to-disk.sh` keeps the parts of the RAM system | SSH key, feedback tools, `ramboot-agent`. Options `--firmware auto\|all\|none` and `--desktop` (no snap transitional packages) |
