# esp32-ramboot

An ESP32-S3 board starts a computer into Ubuntu Server in RAM.
A second ESP32-S3 board gives remote control of the first board through Bluetooth LE.

## Description

- The **device** board connects to a USB port of the target computer.
  It is a USB keyboard, a USB mouse, and a read-only USB disk.
- The **bridge** board connects to a USB port of the host computer.
  It sends the commands of the host computer to the device board through Bluetooth LE.

The sequence on the target computer is:

1. The UEFI firmware starts Stage 1 from the USB disk.
   Stage 1 is a Linux kernel with BusyBox (3.4 MB).
2. Stage 1 gets an IP address through the wired network adapter.
3. Stage 1 downloads Ubuntu Server (385 MB) and does a check of the SHA-256 values.
4. Stage 1 starts Ubuntu Server with `kexec`.
5. Ubuntu Server 26.04 operates in RAM.
   It does not write data to the disks of the target computer.

## Limits

- The target computer must have a wired network adapter from Intel, Realtek, Broadcom, or Atheros.
  Stage 1 has no drivers for wireless network adapters.
- The network must have a DHCP server and access to the download location.
- The target computer must start in UEFI mode. Secure Boot must be off.
- The target computer must have a minimum of 2 GB of RAM.
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

> **CAUTION:** Do not put `kvm_secrets.h`, `boards.env`, or `boot/keys/` into the repository.
> A person who has the token can send keys to the target computer.

### 2. Make the boot disk image

1. Download `linux-6.18.60.tar.xz` from <https://cdn.kernel.org/pub/linux/kernel/v6.x/>.
2. Put the file into `boot/dl/`.
3. Enter these commands:

   ```sh
   cd boot
   ./setup-tools.sh     # local copies of QEMU, OVMF, and BusyBox
   ./fetch-ubuntu.sh    # Ubuntu files, 390 MB
   ./build.sh           # result: out/stage1.img, 8 MiB
   ```

Stage 1 downloads Ubuntu Server from `cloud-images.ubuntu.com`.
To use a different download location, give its address to `build.sh` as a parameter.

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
Attach the device board again each time it starts.

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

| Command | Function |
|---|---|
| `kvm.py key ctrl+alt+delete` | Pushes a key or a key combination |
| `kvm.py type 'text' [--enter]` | Types text (US keyboard layout) |
| `kvm.py repeat F12 15000 100` | Pushes F12 each 100 ms for 15 seconds |
| `kvm.py move X Y` | Moves the mouse pointer. X and Y are 0 to 32767 |
| `kvm.py click X Y [left\|right\|middle]` | Clicks a mouse button |
| `kvm.py scroll N` | Turns the mouse wheel |
| `kvm.py mode [hid\|hid+msc\|msc] [save]` | Shows or changes the USB mode |
| `kvm.py wait <text> [seconds]` | Waits for an event from the device board |
| `kvm.py log` | Shows the log of the device board |
| `kvm.py dfu` | Puts the device board into download mode |

The default USB mode is `hid+msc`: keyboard, mouse, and boot disk.
Use the mode `msc` if the UEFI firmware does not start from the default mode.

If the link to the host computer stops, the device board releases all keys in 3 seconds.

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
   Write the image for QEMU to the device board first.
3. Monitor `out/serial.log` until the line `ramboot login:` shows.
4. Log in with `./ssh-vm.sh`.
5. Stop QEMU.
6. Make sure that the test disk did not change:

   ```sh
   sha256sum -c out/fakedisk.sha256
   ```

### 6. Start a target computer

**Note:** The author did not do this procedure on a real computer.

1. Make the image with `./build.sh`. Write it to the device board.
2. Connect a network cable to the target computer.
3. Connect the native USB port of the device board to the target computer.
4. Start the target computer.
5. Select the USB disk `DIY S3 Boot Disk` in the boot menu.
   For example, `python3 tools/kvm.py repeat F12 15000 100` pushes F12 for 15 seconds.
6. Get the IP address of the target computer from the DHCP server.
7. Log in:

   ```sh
   ssh -i boot/keys/id_ed25519 ubuntu@<IP address>
   ```

## Test status

The author did these tests on 2026-11-25 with two boards and QEMU 8.2:

- QEMU started Ubuntu Server 26.04.1 from the image file and from the device board.
- The target wrote no data to its disks. The SHA-256 value of the test disk did not change.
- The host computer read the boot disk at 805 kB/s. The SHA-256 value was correct.
- The target received the correct keys and mouse events.
- The device board stopped the keys when the link stopped.
- The firmware files and the boot disk image contain no paths or names of the host computer.

The author did not do these tests:

- Start of a real computer.
- Download from `cloud-images.ubuntu.com` in Stage 1. The tests used a local HTTP server.
- First pairing of two new boards.

## Known problems

- The mouse commands give the error `usb:ESP_ERR_TIMEOUT` if no program on the target reads the mouse.
- If the host changes the USB driver without a bus reset, the boot disk can stop.
  The option `REAL=port` of `run-qemu.sh` prevents this problem in QEMU.
- If Stage 1 finds no network, it ejects the boot disk and waits for 180 seconds.
  The host computer receives the event `msc eject`.

## Source code

- `boot/`: scripts for the boot disk image and the QEMU test.
- `firmware/device/` and `firmware/bridge/`: firmware for the two boards.
- `firmware/components/`: the code of the device board, the protocol constants, and TinyUSB.
- `tools/` and `scripts/`: host tool, test tools, and installation scripts.

The comments in the source files and the messages of the tools are in Chinese.

A different project can add its own commands to the device firmware.
Refer to `kvm_cmd.h` and `kvm_link.h` in `firmware/components/kvm_core/include/`.

## License

This project has the MIT License. Refer to `LICENSE`.
TinyUSB in `firmware/components/espressif__tinyusb/` has its own MIT License.
