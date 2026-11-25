# esp32-ramboot

An ESP32-S3 board starts a computer into Ubuntu Server in RAM.
A second ESP32-S3 board gives remote control of the first board through Bluetooth LE.

## Description

- The **device** board connects to a USB port of the target computer.
  It is a USB keyboard, a USB mouse, and a read-only USB disk.
  It can also be a USB network adapter that uses the WiFi of the board.
- The **bridge** board connects to a USB port of the host computer.
  It sends the commands of the host computer to the device board through Bluetooth LE.

The sequence on the target computer is:

1. The UEFI firmware or the BIOS starts Stage 1 from the USB disk.
   Stage 1 is a Linux kernel with BusyBox (3.4 MB).
2. Stage 1 gets an IP address through a network adapter.
3. Stage 1 downloads Ubuntu Server (386 MB) and does a check of the SHA-256 values.
4. Stage 1 starts Ubuntu Server with `kexec`.
5. Ubuntu Server 26.04 operates in RAM.
   It does not write data to the disks of the target computer.

Stage 1 and Ubuntu Server send their progress to the device board through the USB disk.
The device board sends this data to the host computer.
Thus a screen on the target computer is not necessary.

## Limits

- The target computer must have a 64-bit x86 processor and a minimum of 2 GB of RAM.
- The target computer must start from USB in UEFI mode or in BIOS mode. Secure Boot must be off.
- The target computer must have one of these network connections:
  - A wired network adapter from Intel, Realtek, Broadcom, or Atheros.
  - A USB network adapter (ASIX, Realtek, CDC) or a telephone with USB tethering.
  - The device board in the USB mode `net`. The rate is approximately 550 kB/s.
- Stage 1 has no drivers for wireless network adapters.
- The network must have a DHCP server and access to the download location.
- The host computer gets no data from the screen of the target computer.
- The boards must have 16 MB of flash memory. The maximum size of the boot disk image is 12 MB.

## Hardware

- Two ESP32-S3 boards with 16 MB of flash memory and a native USB port. PSRAM is not necessary.
- One USB cable for each board.

The author used Freenove ESP32-S3-WROOM CAM boards.

> **CAUTION:** Do not connect the two USB ports of a Freenove board to two different power sources.
> The VBUS pins of the two ports connect directly, and current can flow between the power sources.
> If you must use the two ports at the same time, cut the VBUS wire of one cable.

## Software

- Linux host computer. The author used Ubuntu 24.04 in WSL2.
- ESP-IDF v5.3.2. The default location is `~/esp/esp-idf`. Set `IDF_PATH` for a different location.
- Python 3 with `pyserial`.
- `gcc`, `make`, `flex`, `bison`, `bc`, `xz`, `cpio`, `wget`, `mtools`, `dosfstools`, `gdisk`,
  `e2fsprogs`, and `openssh-client`.

## Procedures

### 1. Prepare the repository

1. Make the pairing passkey and the token:

   ```sh
   bash scripts/gen_secrets.sh
   ```

2. Make a copy of `boards.env.example` with the name `boards.env`.
3. Write the MAC addresses of the two boards into `boards.env`.
   The command `ls /dev/serial/by-id/` shows the MAC address of a connected board.
4. For the USB mode `net`, write the WiFi data into the file `Wifi.txt`. Then enter:

   ```sh
   python3 scripts/gen_wifi_secrets.py
   ```

   The comment at the top of the script gives the format of `Wifi.txt`.

> **CAUTION:** Do not put `kvm_secrets.h`, `wifi_secrets.h`, `Wifi.txt`, `boards.env`, or `boot/keys/`
> into the repository.
> A person who has the token can send keys to the target computer.
> The firmware file contains the WiFi password. Do not give the firmware file to other persons.

### 2. Make the boot disk image

1. Download `linux-6.18.60.tar.xz` from <https://cdn.kernel.org/pub/linux/kernel/v6.x/>.
2. Put the file into `boot/dl/`.
3. Enter these commands:

   ```sh
   cd boot
   ./setup-tools.sh     # local copies of QEMU, OVMF, syslinux, and BusyBox
   ./fetch-ubuntu.sh    # Ubuntu files, 390 MB
   ./build.sh           # result: out/stage1.img, 8 MiB
   ```

The image has an MBR partition table and one FAT16 partition.
It contains a boot loader for UEFI and a boot loader for BIOS.

`build.sh` also makes an SSH key pair in `boot/keys/`.
Use this key to log in to Ubuntu Server as the user `ubuntu`.

### 3. Install the firmware

1. Connect the native USB port of the bridge board to the host computer.
2. Install the bridge firmware:

   ```sh
   bash scripts/flash_local.sh bridge
   ```

3. Connect the native USB port of the device board to the host computer.
4. Hold the BOOT button and push the RST button. Then release the BOOT button.
5. Install the device firmware and the boot disk image:

   ```sh
   bash scripts/flash_local.sh device
   ```

After the first installation, the BOOT button is not necessary.
The command `python3 tools/kvm.py dfu` puts the device board into download mode.

To write only the boot disk image, enter `bash scripts/flash_local.sh device img`.

**Note:** In WSL2, attach each board with `usbipd attach --wsl --busid <id>` in Windows.
Attach the device board again each time it starts or changes its USB mode.

### 4. Send commands to the device board

1. Set the serial port of the bridge board:

   ```sh
   export KVM_PORT=/dev/serial/by-id/<port of the bridge board>
   ```

2. Supply power to the device board.
3. Get the status of the two boards:

   ```sh
   python3 tools/kvm.py status
   ```

The two boards pair at the first connection. The pairing stays in the flash memory.
If the link to the host computer stops, the device board releases all keys in 3 seconds.

| Command | Function |
|---|---|
| `kvm.py key ctrl+alt+delete` | Pushes a key or a key combination |
| `kvm.py type 'text' [--enter]` | Types text (US keyboard layout) |
| `kvm.py repeat F12 15000 100` | Pushes F12 each 100 ms for 15 seconds |
| `kvm.py move X Y` | Moves the mouse pointer. X and Y are 0 to 32767 |
| `kvm.py click X Y [left\|right\|middle]` | Clicks a mouse button |
| `kvm.py scroll N` | Turns the mouse wheel |
| `kvm.py mode [hid\|hid+msc\|msc\|net] [save]` | Shows or changes the USB mode |
| `kvm.py wifi [home\|eap]` | Selects the WiFi data. `eap` is for WPA2-Enterprise |
| `kvm.py net` | Shows the status of the USB network adapter |
| `kvm.py wait <text> [seconds]` | Waits for an event from the device board |
| `kvm.py target` | Shows the progress data that the target computer sent |
| `kvm.py mtrace` | Shows which sectors of the boot disk the target computer read |
| `kvm.py cfg [set key=value \| del key \| clear]` | Shows or changes the configuration for the target computer |
| `kvm.py log` | Shows the log of the device board |
| `kvm.py dfu` | Puts the device board into download mode |

The USB modes are:

| Mode | Function of the device board |
|---|---|
| `hid+msc` | Keyboard, mouse, and boot disk. This is the default mode |
| `hid` | Keyboard and mouse. Use it when the target computer must start from its own disk |
| `msc` | Boot disk only. Use it if the target computer does not start from the default mode |
| `net` | Keyboard, boot disk, and USB network adapter. The board connects to the WiFi network |

The device board keeps the configuration in its flash memory. Stage 1 reads it at each start.

| Key | Function |
|---|---|
| `base=<address>` | Download location. The SHA-256 values in the boot disk image stay applicable |
| `cmdline=<parameters>` | More parameters for the Ubuntu kernel |
| `netwait=<seconds>` | Time that Stage 1 waits for a network adapter |

The parameter `ramboot.console=1` logs in the user `root` on the first console of Ubuntu Server.
Then you can type commands with `kvm.py type`.
The command `r <command>` on the target computer sends the output of `<command>` to the host computer.

> **CAUTION:** Use `ramboot.console=1` only when it is necessary.
> A person who has access to the keyboard of the target computer gets `root` access.

### 5. Do a test in QEMU

1. Make the image for QEMU:

   ```sh
   ./build.sh http://10.0.2.2:8000/ubuntu
   ```

2. Start QEMU:

   ```sh
   ./run-qemu.sh
   ```

   To use the device board as the boot disk, enter `REAL=port ./run-qemu.sh`.
   The parameter `BIOS=1` uses SeaBIOS. The parameter `NIC=none` removes the network adapter.
3. Monitor `out/serial.log` until the line `ramboot login:` shows.
4. Log in with `./ssh-vm.sh`.
5. Stop QEMU.
6. Make sure that the test disk did not change:

   ```sh
   sha256sum -c out/fakedisk.sha256
   ```

### 6. Start a target computer

1. Make the image with `./build.sh`. Write it to the device board.
2. Connect the native USB port of the device board to the target computer.
3. Start the target computer.
   If it does not start from the USB disk, select `DIY S3 Boot Disk` in the boot menu.
   For example, `python3 tools/kvm.py repeat F12 15000 100` pushes F12 for 15 seconds.
4. Monitor the progress:

   ```sh
   python3 tools/kvm.py wait "s1 network" 120
   ```

   Stage 1 sends the computer model, the network adapters, and the network status.
5. If Stage 1 sends `no network`, connect a network cable or enter `python3 tools/kvm.py mode net`.
6. Wait for Ubuntu Server:

   ```sh
   python3 tools/kvm.py wait "os disk" 1800
   ```

7. Enter `python3 tools/kvm.py target` to get the IP address and the disk list.
8. Log in with `ssh -i boot/keys/id_ed25519 ubuntu@<IP address>`.
   If the host computer cannot connect, use `ramboot.console=1` and `kvm.py type`.

### 7. Install Ubuntu Server on a disk of the target computer

> **WARNING:** This procedure erases all data on the disk.
> Before you start, make sure that the disk has no data that is necessary.
> The command `lsblk -f` shows the partitions and the file systems of each disk.

1. Do procedure 6.
2. Copy `tools/install-to-disk.sh` to the target computer.
3. Enter this command as `root` on the target computer:

   ```sh
   sh install-to-disk.sh /dev/sdX <user> <password> <hostname>
   ```

   The script copies the system from RAM to the disk.
   Then it downloads the kernel and the boot loaders (approximately 200 MB).
4. When the script sends `done`, enter `python3 tools/kvm.py mode hid`.
5. Start the target computer again.

The parameter `--boot-only` installs the boot loaders again on a disk that the script installed.

## Test status

Date of the tests: 2026-11-25.

Tests in QEMU 8.2 with the two boards:

- OVMF and SeaBIOS started Ubuntu Server 26.04.1 from the device board.
- The target wrote no data to its disks. The SHA-256 value of the test disk did not change.
- The host computer read the boot disk at 805 kB/s. The SHA-256 value was correct.
- The target received the correct keys and mouse events.
- The device board stopped the keys when the link stopped.
- The firmware files and the boot disk image contain no paths or names of the host computer.

Test with a Lenovo ThinkCentre M91p (UEFI, 16 GB of RAM, no network cable, no screen at the start):

- The computer started Stage 1 from the device board.
- The host computer received the computer model, the memory size, the network adapter, and its driver.
- In the mode `net`, the board connected to a WPA2-Enterprise network.
  Stage 1 downloaded 386 MB in 704 seconds.
- Ubuntu Server operated in RAM and sent its IP address and its disk list.
- The host computer typed commands as `root` and received their output. No network connection was necessary.
- `install-to-disk.sh` installed Ubuntu Server on the disk. The computer started from the disk.
  The login and `sudo` were correct.

The author did not do these tests:

- A computer that has a BIOS but no UEFI firmware.
- First pairing of two new boards.
- `install-to-disk.sh` on a computer with an NVMe disk.

## Known problems

Problems that the test with the real computer showed:

- **Boot disk format.** The Lenovo firmware did not start a boot disk with GPT and a FAT16 partition.
  It started the same files from a disk with an MBR partition table. QEMU started the two formats.
- **Keys in the mode `net`.** When the host computer sends a key, the USB network adapter on the target stops.
  The keyboard also stops. The cause is not known.
  Use the mode `hid+msc` to type and the mode `net` for the network.
- **Second start of the mode `net`.** It fails with a timeout. Enter `kvm.py reboot` before `kvm.py mode net`.
  A correction is in the source code. The author did not do a test of it on a board.
- **Boot entries.** A system that `kexec` started cannot write UEFI boot entries.
  The Lenovo firmware did not start the installed disk from the usual locations.
  `install-to-disk.sh` puts a copy of the boot loader at the path of the Windows boot manager.
  It also installs a BIOS boot loader. It is not known which of the two the firmware uses.
- **Download time.** Each start downloads 386 MB. Through the device board, the time is 12 minutes.
- **Network access.** The target computer gets an address in the WiFi network of the board.
  A host computer in a different network cannot use SSH.

Other problems:

- The mouse commands give the error `usb:ESP_ERR_TIMEOUT` if no program on the target reads the mouse.
- SeaBIOS uses only the first interface of a composite USB device. It starts only from the mode `msc`.
- If the host changes the USB driver without a bus reset, the boot disk can stop.
  The option `REAL=port` of `run-qemu.sh` prevents this problem in QEMU.
- If Stage 1 finds no network, it ejects the boot disk and waits for 180 seconds.
  The key `netwait` changes this time. The host computer receives the event `msc eject`.

`TODO.md` gives the tasks that are open.

## Source code

- `boot/`: scripts for the boot disk image and the QEMU test.
- `firmware/device/` and `firmware/bridge/`: firmware for the two boards.
- `firmware/components/`: the code of the device board, the protocol constants, and TinyUSB.
- `tools/` and `scripts/`: host tool, installation script for the target disk, and firmware scripts.

The comments in the source files and the messages of the tools are in Chinese.

A different project can add its own commands to the device firmware.
Refer to `kvm_cmd.h` and `kvm_link.h` in `firmware/components/kvm_core/include/`.

## License

This project has the MIT License. Refer to `LICENSE`.
TinyUSB in `firmware/components/espressif__tinyusb/` has its own MIT License.
