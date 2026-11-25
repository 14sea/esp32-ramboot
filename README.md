# esp32-ramboot

An ESP32-S3 board starts a computer into Ubuntu Server in RAM.
A second ESP32-S3 board gives remote control of the first board through Bluetooth LE.

## Description

This project uses two ESP32-S3 boards.

- The **device** board connects to a USB port of the target computer.
  The target computer sees a USB keyboard, a USB mouse, and a read-only USB disk.
- The **bridge** board connects to a USB port of the host computer.
  It sends the commands of the host computer to the device board through Bluetooth LE.

The USB disk contains a small Linux system (Stage 1).
The target computer starts from this disk.
Stage 1 uses the wired network adapter of the target computer to download Ubuntu Server.
Then Stage 1 starts Ubuntu Server with `kexec`.
Ubuntu Server operates fully in RAM.
The software does not write data to the disks of the target computer.

```
host computer --USB serial--> bridge board --Bluetooth LE--> device board --USB--> target computer
tools/kvm.py                  firmware/bridge                firmware/device       keyboard, mouse,
                                                                                   read-only boot disk

Sequence on the target computer:
  1. The UEFI firmware starts Stage 1 from the boot disk (Linux kernel and BusyBox, 3.4 MB).
  2. Stage 1 gets an IP address from the DHCP server.
  3. Stage 1 downloads the Ubuntu kernel, the initrd, and the root file system (385 MB).
  4. Stage 1 compares each file with its SHA-256 value.
  5. Stage 1 starts the Ubuntu kernel with kexec.
  6. Ubuntu Server 26.04 operates in RAM. You can log in with SSH.
```

## Limits

- The target computer must have a wired network adapter.
  Stage 1 has drivers for these adapters: Intel (e1000, e1000e, igb, igc), Realtek (8139, r8169),
  Broadcom (tg3), Atheros (atl1c, alx), and virtio.
- Stage 1 does not have drivers for wireless network adapters.
- The network must have a DHCP server.
  The target computer must have access to the download location.
- The target computer must start in UEFI mode.
  Secure Boot must be off.
- The target computer must have a minimum of 2 GB of RAM. 4 GB is better.
- The device board has no camera.
  The host computer cannot see the screen of the target computer.
- The boards must have 16 MB of flash memory.
  The boot disk image can have a maximum size of 12 MB.

## Hardware

- Two ESP32-S3 boards with 16 MB of flash memory and a native USB port.
  PSRAM is not necessary.
- One USB cable for each board.

The author developed the project with Freenove ESP32-S3-WROOM CAM boards.
These boards have two USB ports: the native USB port and a USB-to-serial port.

> **CAUTION:** On the Freenove board, the VBUS pins of the two USB ports connect directly.
> Do not connect the two ports to two different power sources.
> Current can flow from one power source into the other and cause damage.
> If you must use the two ports at the same time, cut the VBUS wire of one cable.

## Contents of the repository

| Path | Contents |
|---|---|
| `boot/` | Scripts that build the boot disk image, and the QEMU test |
| `boot/stage1/` | Stage 1: kernel configuration, `init` script, `s1tool` |
| `boot/stage2/` | Configuration for Ubuntu Server: dracut hook, cloud-init data |
| `firmware/device/` | Firmware for the device board |
| `firmware/bridge/` | Firmware for the bridge board |
| `firmware/components/kvm_core/` | Command layer, Bluetooth LE, USB keyboard, USB mouse, USB disk |
| `firmware/components/kvm_common/` | Protocol constants and secrets for the two boards |
| `firmware/components/espressif__tinyusb/` | TinyUSB 0.21.0 with one change (refer to `README.kvm.md` in that directory) |
| `tools/kvm.py` | Host tool that sends commands to the device board |
| `tools/serial_relay.py` | Relay for a bridge board that connects to a different computer |
| `tools/evgrab.py` | Test tool that records the keys that the target computer receives |
| `scripts/` | Scripts that make the secrets and install the firmware |

The comments in the source files and the messages of the tools are in Chinese.

## Necessary software

- A Linux host computer. The author used Ubuntu 24.04 in WSL2.
- ESP-IDF v5.3.2. The scripts use `$IDF_PATH`. The default is `~/esp/esp-idf`.
- Python 3 with `pyserial`.
- `gcc`, `make`, `flex`, `bison`, `bc`, `xz`, `cpio`, `wget`, `mtools`, `dosfstools`, `gdisk`,
  `e2fsprogs`, and `openssh-client`.
- QEMU and OVMF. The script `boot/setup-tools.sh` installs local copies. Root access is not necessary.

## Procedures

### 1. Prepare the repository

1. Make the secrets:

   ```sh
   bash scripts/gen_secrets.sh
   ```

   The script writes the pairing passkey and the token to `kvm_secrets.h`.
   The two boards and `tools/kvm.py` use the same file.
2. Copy `boards.env.example` to `boards.env`.
3. Connect each board to the host computer and run `ls /dev/serial/by-id/`.
   The name of the port contains the MAC address of the board.
4. Write the two MAC addresses into `boards.env`.

> **CAUTION:** Do not put `kvm_secrets.h`, `boards.env`, or `boot/keys/` into the repository.
> A person who has the token can send keys to the target computer.

### 2. Build the boot disk image

1. Go to the `boot` directory.
2. Install the local tools:

   ```sh
   ./setup-tools.sh
   ```

3. Download the Ubuntu files (390 MB):

   ```sh
   ./fetch-ubuntu.sh
   ```

4. Download `linux-6.18.60.tar.xz` from <https://cdn.kernel.org/pub/linux/kernel/v6.x/>.
   Put the file into `boot/dl/`.
5. Build the image:

   ```sh
   ./build.sh
   ```

   The time for the first build is approximately 2 minutes.
   The result is `boot/out/stage1.img` (8 MiB).

Without a parameter, Stage 1 downloads Ubuntu Server from `cloud-images.ubuntu.com`.
To use a different download location, give its address as the parameter.
For the QEMU test, use `./build.sh http://10.0.2.2:8000/ubuntu`.

The script also makes an SSH key pair in `boot/keys/`.
Ubuntu Server accepts only this key for the user `ubuntu`.

### 3. Install the firmware

Install the firmware on the bridge board:

1. Connect the native USB port of the bridge board to the host computer.
2. Run:

   ```sh
   bash scripts/flash_local.sh bridge
   ```

Install the firmware on the device board:

1. Connect the native USB port of the device board to the host computer.
2. Hold the BOOT button and push the RST button. Then release the BOOT button.
   The board goes into download mode.
3. Run:

   ```sh
   bash scripts/flash_local.sh device
   ```

   The script writes the firmware and the boot disk image.

After the first installation, the BOOT button is not necessary.
This command puts the device board into download mode through the bridge board:

```sh
python3 tools/kvm.py dfu
```

To write only the boot disk image, run `bash scripts/flash_local.sh device img`.

**Note:** In WSL2, attach each board with `usbipd attach --wsl --busid <id>` in Windows.
The device board becomes a new USB device each time it starts again. Attach it again.

### 4. Connect the host computer to the device board

1. Set the serial port of the bridge board:

   ```sh
   export KVM_PORT=/dev/serial/by-id/<port of the bridge board>
   ```

2. Supply power to the device board.
3. Run:

   ```sh
   python3 tools/kvm.py status
   ```

   The first connection pairs the two boards. The pairing stays in the flash memory.

### 5. Do a test in QEMU without a board

1. Build the image with `./build.sh http://10.0.2.2:8000/ubuntu`.
2. Start QEMU:

   ```sh
   ./run-qemu.sh
   ```

3. Monitor `out/serial.log` until you see the line `ramboot login:`.
4. Log in:

   ```sh
   ./ssh-vm.sh
   ```

5. Stop QEMU. Then make sure that the test disk did not change:

   ```sh
   sha256sum -c out/fakedisk.sha256
   ```

### 6. Do a test in QEMU with the device board

1. Do the procedures 2, 3, and 4 with the image for QEMU.
2. On WSL2, install the access rule one time: `sudo sh scripts/wsl_usb_perms.sh`.
3. Start QEMU with the device board as the boot disk:

   ```sh
   REAL=port ./run-qemu.sh
   ```

4. Continue with step 3 of procedure 5.

### 7. Start a target computer

**Note:** The author did not do this procedure on a real computer. Refer to "Test results".

1. Build the image without a parameter. Write it to the device board.
2. Connect a network cable to the target computer.
3. Connect the native USB port of the device board to the target computer.
4. Start the target computer.
5. Select the USB disk `DIY S3 Boot Disk` in the boot menu of the target computer.
   For example, this command pushes F12 for 15 seconds:

   ```sh
   python3 tools/kvm.py repeat F12 15000 100
   ```

6. Wait until Ubuntu Server starts. Stage 1 shows its progress on the screen.
7. Get the IP address of the target computer from the DHCP server.
8. Log in:

   ```sh
   ssh -i boot/keys/id_ed25519 ubuntu@<IP address>
   ```

## Commands of the host tool

| Command | Function |
|---|---|
| `kvm.py status` | Shows the status of the two boards |
| `kvm.py key ctrl+alt+delete` | Pushes a key or a key combination |
| `kvm.py type 'text' [--enter]` | Types text (US keyboard layout) |
| `kvm.py repeat F12 15000 100` | Pushes F12 each 100 ms for 15 seconds |
| `kvm.py move X Y` | Moves the mouse pointer. X and Y are 0 to 32767 |
| `kvm.py click X Y [left\|right\|middle] [count]` | Clicks a mouse button |
| `kvm.py scroll N` | Turns the mouse wheel |
| `kvm.py drag X1 Y1 X2 Y2` | Moves the mouse pointer with the button held |
| `kvm.py mode [hid\|hid+msc\|msc] [save]` | Shows or changes the USB mode. `save` keeps the mode after a restart |
| `kvm.py wait <text> [seconds]` | Waits for an event from the device board |
| `kvm.py log` | Shows the log of the device board |
| `kvm.py dfu` | Puts the device board into download mode |
| `kvm.py reboot` | Starts the device board again |
| `kvm.py bridge @status` | Sends a local command to the bridge board |

The USB modes are:

- `hid+msc`: keyboard, mouse, and boot disk. This is the default mode.
- `hid`: keyboard and mouse.
- `msc`: boot disk only. Use this mode if the UEFI firmware does not accept the default mode.

If the host computer stops, the device board releases all keys after 3 seconds.

## Additions to the firmware

A different project can use `firmware/components/` and add its own commands.
The interface is in `kvm_cmd.h` and `kvm_link.h`.

- `kvm_cmd_register()` adds a command.
- `kvm_cmd_start_job()` starts a long task that the host computer can stop.
- `kvm_cmd_register_info()` adds data to the `state` and `caps` replies.
- `kvm_link_set_alt()` replaces Bluetooth LE with a different link.

Do these calls between `kvm_cmd_init()` and `kvm_ble_start()`.
Only the commands of this project use this interface at this time.

## Test results

Date of the tests: 2026-11-25.
Host computer: Ubuntu 24.04 in WSL2. QEMU 8.2 operated without hardware acceleration.
The device board connected to WSL2 through `usbipd`.

| Test | Result |
|---|---|
| Build of the two firmware projects | Device: 594 kB. Bridge: 544 kB |
| Build of the boot disk image in an empty directory | Stage 1 is 3,388,416 bytes. The image is 8 MiB |
| QEMU without a board (procedure 5) | Ubuntu Server 26.04.1 started. SSH login was correct |
| Read of the boot disk on the host computer | 8 MiB at 805 kB/s. The SHA-256 value was correct |
| QEMU with the device board (procedure 6) | UEFI started Stage 1 from the board. Ubuntu Server started |
| Disks of the target | 0 writes. No swap. The SHA-256 value of the test disk did not change |
| `type` with all printable characters, `key F12 down up` | The target received the same text and keys |
| Mouse: `move`, `click`, `scroll`, `drag` | The target received the correct events |
| Link stop while `repeat` operated | The device board stopped the keys in less than 1 second |
| `eject` on the target | The host computer received the event `msc eject` |
| `mode hid`, then `mode hid+msc` | The target saw the disk again. The SHA-256 value was correct |
| `dfu`, then installation of the firmware | Correct. The pairing stayed |
| Download locations at `cloud-images.ubuntu.com` | The three files are available |
| Search for local data in the firmware files and the boot disk image | No paths, user name, or host name of the build computer |

These tests were not done:

- Start of a real computer (procedure 7). QEMU with OVMF accepts more than real UEFI firmware.
- Download from `cloud-images.ubuntu.com` in Stage 1. The tests used a local HTTP server.
- First pairing of two new boards. The test boards had a pairing from a previous firmware version.
- The interface for additions to the firmware. No project uses it at this time.

## Known problems

- If the host changes the USB driver without a bus reset, the boot disk can stop.
  For this reason, the option `guest-resets-all=on` is necessary in QEMU. `REAL=port` sets this option.
- The mouse commands give the error `usb:ESP_ERR_TIMEOUT` if no program on the target reads the mouse.
  This occurs on a text console.
- In WSL2, the device board disconnects from WSL when it changes its USB mode. Attach it again.
- The serial relay stops when the bridge board disconnects from its USB hub. Start it again.
- If Stage 1 finds no network, it ejects the boot disk and waits for 180 seconds.
  The device board sends the event `msc eject` to the host computer.

## License

This project has the MIT License. Refer to `LICENSE`.
The TinyUSB source code in `firmware/components/espressif__tinyusb/` has its own MIT License.
Refer to the `LICENSE` file in that directory.
