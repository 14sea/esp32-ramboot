# esp32-ramboot

An ESP32-S3 board starts a computer into Ubuntu Server in RAM.
A second ESP32-S3 board gives remote control of the first board through Bluetooth LE.

## Description

- The **device** board connects to a USB port of the target computer.
  It is a USB keyboard, a USB mouse, a read-only USB disk, and a USB network adapter.
- The **bridge** board connects to a USB port of the host computer.
  It sends the commands of the host computer to the device board through Bluetooth LE.

The sequence on the target computer is:

1. The UEFI firmware or the BIOS starts Stage 1 from the USB disk (Linux kernel and BusyBox, 3.4 MB).
2. Stage 1 gets an IP address, downloads Ubuntu Server (386 MB), and does a check of the SHA-256 values.
3. Stage 1 starts Ubuntu Server with `kexec`.
4. Ubuntu Server 26.04 operates in RAM. It does not write data to the disks of the target computer.

The target computer sends its progress to the host computer through the device board.
A screen on the target computer is not necessary.

## Limits

- Target computer: 64-bit x86 processor, 2 GB of RAM or more, start from USB, Secure Boot off.
- Network of the target computer: a wired network adapter, a USB network adapter, or the device board.
  Stage 1 has no drivers for wireless network adapters.
- The network must have a DHCP server and access to the download location.
- Boards: 16 MB of flash memory and a native USB port. The author used Freenove ESP32-S3-WROOM CAM boards.

> **CAUTION:** Do not connect the two USB ports of a Freenove board to two different power sources.
> The VBUS pins of the two ports connect directly, and current can flow between the power sources.

## Software

- Linux host computer. The author used Ubuntu 24.04 in WSL2.
- ESP-IDF v5.3.2 at `~/esp/esp-idf`, or set `IDF_PATH`.
- Python 3 with `pyserial`.
- `gcc`, `make`, `flex`, `bison`, `bc`, `xz`, `cpio`, `wget`, `mtools`, `dosfstools`, `gdisk`,
  `e2fsprogs`, and `openssh-client`.

## Procedures

### 1. Prepare the repository

1. Enter `bash scripts/gen_secrets.sh` to make the pairing passkey and the token.
2. Make a copy of `boards.env.example` with the name `boards.env`.
3. Write the MAC addresses of the two boards into `boards.env`.
   The command `ls /dev/serial/by-id/` shows the MAC address of a connected board.
4. For the USB mode `net`, write the WiFi data into `Wifi.txt` and enter `python3 scripts/gen_wifi_secrets.py`.
   The comment at the top of the script gives the format of `Wifi.txt`.

> **CAUTION:** Do not put `kvm_secrets.h`, `wifi_secrets.h`, `Wifi.txt`, `boards.env`, or `boot/keys/`
> into the repository.
> The firmware file contains the WiFi password. Do not give the firmware file to other persons.

### 2. Make the boot disk image

1. Download `linux-6.18.60.tar.xz` from <https://cdn.kernel.org/pub/linux/kernel/v6.x/>.
2. Put the file into `boot/dl/`.
3. Enter these commands:

   ```sh
   cd boot
   ./setup-tools.sh     # local copies of QEMU, OVMF, syslinux, and BusyBox
   ./fetch-ubuntu.sh    # Ubuntu files, 390 MB
   ./build.sh           # result: out/stage1.img, 8 MiB, for UEFI and BIOS
   ```

`build.sh` also makes an SSH key pair in `boot/keys/` for the user `ubuntu`.

### 3. Install the firmware

1. Connect the native USB port of the bridge board to the host computer.
2. Enter `bash scripts/flash_local.sh bridge`.
3. Connect the native USB port of the device board to the host computer.
4. Hold the BOOT button and push the RST button. Then release the BOOT button.
5. Enter `bash scripts/flash_local.sh device`. The script writes the firmware and the boot disk image.

After the first installation, `python3 tools/kvm.py dfu` puts the device board into download mode.

**Note:** In WSL2, attach each board with `usbipd attach --wsl --busid <id>` in Windows.
Attach the device board again each time it starts or changes its USB mode.

### 4. Send commands to the device board

1. Set the serial port of the bridge board:

   ```sh
   export KVM_PORT=/dev/serial/by-id/<port of the bridge board>
   ```

2. Supply power to the device board.
3. Enter `python3 tools/kvm.py status`. The two boards pair at the first connection.

| Command | Function |
|---|---|
| `kvm.py key ctrl+alt+delete` | Pushes a key or a key combination |
| `kvm.py type 'text' [--enter]` | Types text (US keyboard layout) |
| `kvm.py repeat F12 15000 100` | Pushes F12 each 100 ms for 15 seconds |
| `kvm.py move X Y`, `click X Y`, `scroll N` | Controls the mouse. X and Y are 0 to 32767 |
| `kvm.py mode [hid\|hid+msc\|msc\|net]` | Shows or changes the USB mode |
| `kvm.py wifi [home\|eap]` | Selects the WiFi data. `eap` is for WPA2-Enterprise |
| `kvm.py wait <text> [seconds]` | Waits for an event from the device board |
| `kvm.py target` | Shows the progress data that the target computer sent |
| `kvm.py mtrace` | Shows which sectors of the boot disk the target computer read |
| `kvm.py cfg [set key=value \| del key]` | Shows or changes the configuration for the target computer |

| USB mode | Function of the device board |
|---|---|
| `hid+msc` | Keyboard, mouse, and boot disk. This is the default mode |
| `hid` | Keyboard and mouse. The target computer starts from its own disk |
| `msc` | Boot disk only, for a target computer that does not start from the default mode |
| `net` | Keyboard, boot disk, and USB network adapter through the WiFi of the board |

Stage 1 reads the configuration from the device board at each start. The keys are:

- `base=<address>`: download location.
- `cmdline=<parameters>`: more parameters for the Ubuntu kernel.
- `netwait=<seconds>`: time that Stage 1 waits for a network adapter.

The parameter `ramboot.console=1` logs in the user `root` on the first console of Ubuntu Server.
Then `kvm.py type 'r <command>' --enter` sends the output of `<command>` to the host computer.

> **CAUTION:** With `ramboot.console=1`, a person at the keyboard of the target computer gets `root` access.

### 5. Do a test in QEMU

1. Enter `./build.sh http://10.0.2.2:8000/ubuntu` to make the image for QEMU.
2. Enter `./run-qemu.sh`.
   `REAL=port` uses the device board as the boot disk. `BIOS=1` uses SeaBIOS. `NIC=none` removes the network adapter.
3. Monitor `out/serial.log` until the line `ramboot login:` shows.
4. Log in with `./ssh-vm.sh`.
5. Stop QEMU. Then enter `sha256sum -c out/fakedisk.sha256` to make sure that the test disk did not change.

### 6. Start a target computer

1. Make the image with `./build.sh` and write it to the device board.
2. Connect the native USB port of the device board to the target computer.
3. Start the target computer.
   If it does not start from the USB disk, use `kvm.py repeat F12 15000 100` and select `DIY S3 Boot Disk`.
4. Enter `python3 tools/kvm.py wait "s1 network" 120` to monitor Stage 1.
5. If Stage 1 sends `no network`, connect a network cable or enter `python3 tools/kvm.py mode net`.
6. Enter `python3 tools/kvm.py wait "os disk" 1800` to wait for Ubuntu Server.
7. Enter `python3 tools/kvm.py target` to get the IP address and the disk list.
8. Log in with `ssh -i boot/keys/id_ed25519 ubuntu@<IP address>`, or use `ramboot.console=1`.

### 7. Install Ubuntu Server on a disk of the target computer

> **WARNING:** This procedure erases all data on the disk.
> Before you start, enter `lsblk -f` and make sure that the disk has no data that is necessary.

1. Do procedure 6.
2. Copy `tools/install-to-disk.sh` to the target computer.
3. Enter `sh install-to-disk.sh /dev/sdX <user> <password> <hostname>` as `root` on the target computer.
   The script copies the system from RAM and downloads the kernel and the boot loaders (200 MB).
4. When the script sends `done`, enter `python3 tools/kvm.py mode hid`.
5. Start the target computer again.

## Test status

The author did these tests on 2026-11-25:

- QEMU 8.2 with OVMF and with SeaBIOS started Ubuntu Server from the device board.
  The target wrote no data to its disks.
- A Lenovo ThinkCentre M91p with no network cable and no screen started Stage 1 from the device board.
  In the mode `net`, it downloaded 386 MB in 704 seconds through a WPA2-Enterprise network.
- The host computer received the computer model, the IP address, and the disk list of the ThinkCentre.
  It typed commands as `root` and received their output.
- `install-to-disk.sh` installed Ubuntu Server on the ThinkCentre. The computer started from its disk.

## Known problems

- When the host computer sends a key in the mode `net`, the USB network adapter on the target stops.
  Use the mode `hid+msc` to type and the mode `net` for the network.
- The second start of the mode `net` fails. Enter `kvm.py reboot` before `kvm.py mode net`.
- The Lenovo firmware does not start a boot disk with GPT and a FAT16 partition. The image uses MBR for this reason.
- A system that `kexec` started cannot write UEFI boot entries.
- Each start downloads 386 MB. Through the device board, the time is 12 minutes.
- SeaBIOS starts only from the mode `msc`.
- The mouse commands give `usb:ESP_ERR_TIMEOUT` if no program on the target reads the mouse.

`TODO.md` gives the open tasks and the tests that are not done.

## Source code

- `boot/`: boot disk image and QEMU test.
- `firmware/`: firmware for the two boards. `firmware/components/kvm_core/include/kvm_cmd.h` shows how to add commands.
- `tools/` and `scripts/`: host tool, installation script for the target disk, and firmware scripts.

The comments in the source files and the messages of the tools are in Chinese.

## License

This project has the MIT License. Refer to `LICENSE`.
TinyUSB in `firmware/components/espressif__tinyusb/` has its own MIT License.
