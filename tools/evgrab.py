#!/usr/bin/env python3
"""
在目標 Linux 上獨佔抓取設備那個 USB 鍵盤（"S3 KVM Keyboard"）的按鍵，用來驗收 kvm.py 打了什麼（需要 root）。

EVIOCGRAB 獨佔：抓取期間這個鍵盤的事件只送給本腳本，不會打進目標桌面當前的焦點窗口。
只依賴標準庫，可以直接 scp 到目標上跑：
  sudo python3 evgrab.py [--idle 4] [--max 60]
先打印 READY，之後每個按鍵一行，結束時打印 TEXT: 還原出的文本（美式佈局）。
"""
import argparse
import fcntl
import os
import re
import select
import struct
import sys
import time

NAME = "S3 KVM Keyboard"
EVIOCGRAB = 0x40044590
EV_KEY = 1
FMT = "llHHi"
SIZE = struct.calcsize(FMT)

# evdev 鍵碼 → (無 Shift, 有 Shift)
KEYS = {
    2: "1!", 3: "2@", 4: "3#", 5: "4$", 6: "5%", 7: "6^", 8: "7&", 9: "8*", 10: "9(", 11: "0)",
    12: "-_", 13: "=+", 15: "\t\t", 26: "[{", 27: "]}", 28: "\n\n", 39: ";:", 40: "'\"", 41: "`~",
    43: "\\|", 51: ",<", 52: ".>", 53: "/?", 57: "  ",
}
for i, c in enumerate("qwertyuiop"):
    KEYS[16 + i] = c + c.upper()
for i, c in enumerate("asdfghjkl"):
    KEYS[30 + i] = c + c.upper()
for i, c in enumerate("zxcvbnm"):
    KEYS[44 + i] = c + c.upper()
NAMES = {1: "ESC", 14: "BACKSPACE", 29: "LEFTCTRL", 42: "LEFTSHIFT", 54: "RIGHTSHIFT", 56: "LEFTALT",
         58: "CAPSLOCK", 69: "NUMLOCK", 70: "SCROLLLOCK", 87: "F11", 88: "F12", 97: "RIGHTCTRL",
         100: "RIGHTALT", 102: "HOME", 103: "UP", 104: "PAGEUP", 105: "LEFT", 106: "RIGHT", 107: "END",
         108: "DOWN", 109: "PAGEDOWN", 110: "INSERT", 111: "DELETE", 125: "LEFTMETA", 126: "RIGHTMETA"}
NAMES.update({59 + i: f"F{i + 1}" for i in range(10)})
SHIFTS = {42, 54}


def find_device():
    # 鍵盤和滑鼠兩個接口的設備名一樣（都取 USB 產品串），按 Handlers 裡有沒有 kbd 區分
    with open("/proc/bus/input/devices") as f:
        for block in f.read().split("\n\n"):
            if NAME in block and re.search(r"Handlers=.*\bkbd\b", block):
                m = re.search(r"Handlers=.*?(event\d+)", block)
                if m:
                    return "/dev/input/" + m.group(1)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--idle", type=float, default=4, help="最後一個按鍵後多少秒結束")
    ap.add_argument("--max", type=float, default=60, help="最長抓多少秒")
    a = ap.parse_args()

    dev = find_device()
    if not dev:
        sys.exit(f"找不到「{NAME}」輸入設備")
    fd = os.open(dev, os.O_RDONLY)
    fcntl.ioctl(fd, EVIOCGRAB, 1)
    print(f"READY {dev}", flush=True)

    text, shift, held = [], 0, set()
    start = last = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if now - start > a.max or (text or held or last != start) and now - last > a.idle:
                break
            r, _, _ = select.select([fd], [], [], 0.2)
            if not r:
                continue
            data = os.read(fd, SIZE * 64)
            for off in range(0, len(data) - SIZE + 1, SIZE):
                _, _, typ, code, val = struct.unpack_from(FMT, data, off)
                if typ != EV_KEY or val == 2:       # 跳過自動重複
                    continue
                last = time.monotonic()
                name = NAMES.get(code) or (repr(KEYS[code][0]) if code in KEYS else f"KEY_{code}")
                print(f"{'down' if val else 'up  '} {name}", flush=True)
                if code in SHIFTS:
                    shift += 1 if val else -1
                elif val:
                    held.add(code)
                    if code in KEYS:
                        text.append(KEYS[code][1 if shift > 0 else 0])
                    else:
                        text.append(f"<{name}>")
                else:
                    held.discard(code)
    finally:
        fcntl.ioctl(fd, EVIOCGRAB, 0)
        os.close(fd)
    print("TEXT: " + repr("".join(text)), flush=True)
    if held:
        print(f"STUCK: {sorted(held)}", flush=True)


if __name__ == "__main__":
    main()
