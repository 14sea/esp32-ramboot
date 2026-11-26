#!/usr/bin/env python3
"""
在主機上扮演目標機：設備端板子接在本機（或經 usbipd 接進 WSL）時，不用 root 測啟動盤的廠商 SCSI 命令和鍵盤。

  usbfs_target.py msg  DEV "TEXT"         同 s1tool msg（FB TMSG）：報一條給主機
  usbfs_target.py get  DEV                同 s1tool get（FB TGET）：取主機排的下一條。退出碼 0 有、3 空、1 出錯
  usbfs_target.py getcfg DEV              同 s1tool getcfg（FB GCFG）
  usbfs_target.py next DEV "MODE [秒]"    同 s1tool next（FB NEXT）
  usbfs_target.py keys 秒數               讀這段時間裡鍵盤發出的報告，打印按下的鍵（HID 用法碼，帶 S = 按著 Shift）

DEV 只為和 s1tool 的參數一致，不使用：工具自己按廠商號 303a 找板子。所以 ramboot-agent 可以直接拿它當 s1tool 測：
  RAMBOOT_S1TOOL=tools/usbfs_target.py RAMBOOT_AGENT_DIR=/tmp/agent sh boot/stage2/seed.in/ramroot.d/usr/local/sbin/ramboot-agent

為什麼不用 s1tool：內核只讓有 CAP_SYS_RAWIO 的進程經 SG_IO 發廠商命令（0xFB），WSL 裡沒有 root。
這裡改走 usbfs（/dev/bus/usb/…，scripts/wsl_usb_perms.sh 裝的 udev 規則讓普通用戶可讀寫）：
臨時從 usb-storage（或 usbhid）手裡拿走那個接口，自己走 USB 大容量存儲的 Bulk-Only 協議，用完還回去。
只支持 Linux x86-64（ioctl 編號和結構體佈局按它寫）。
"""
import ctypes as C
import fcntl
import glob
import os
import struct
import sys
import time

# linux/usbdevice_fs.h（x86-64）
USBDEVFS_CONTROL = 0xC0185500
USBDEVFS_BULK = 0xC0185502
USBDEVFS_CLAIMINTERFACE = 0x8004550F
USBDEVFS_RELEASEINTERFACE = 0x80045510
USBDEVFS_IOCTL = 0xC0105512
USBDEVFS_CLEAR_HALT = 0x80045515
USBDEVFS_DISCONNECT = 0x5516          # 經 USBDEVFS_IOCTL 用：讓內核驅動放開接口
USBDEVFS_CONNECT = 0x5517             # 同上：還給內核驅動

LIBC = C.CDLL(None, use_errno=True)
LIBC.ioctl.argtypes = [C.c_int, C.c_ulong, C.c_void_p]


def ioctl_ptr(fd, req, data):
    """帶指針的 ioctl：data 是打包好的結構體。返回值 < 0 時拋 OSError"""
    buf = C.create_string_buffer(data, len(data))
    r = LIBC.ioctl(fd, req, buf)
    if r < 0:
        e = C.get_errno()
        raise OSError(e, os.strerror(e))
    return r


def find(cls):
    """找板子上類別為 cls（"08" 大容量存儲 / "03" HID）的第一個接口：返回 (設備節點, 接口號, IN 端點, OUT 端點或 None)"""
    for d in sorted(glob.glob("/sys/bus/usb/devices/*")):
        try:
            if open(d + "/idVendor").read().strip() != "303a" or open(d + "/idProduct").read().strip() == "1001":
                continue                                    # 1001 = 下載模式的串口，不是設備端固件
            for i in sorted(glob.glob(d + "/*:*")):
                if open(i + "/bInterfaceClass").read().strip() != cls:
                    continue
                eps = [int(open(e + "/bEndpointAddress").read(), 16) for e in glob.glob(i + "/ep_*")]
                ein = next(e for e in eps if e & 0x80)
                eout = next((e for e in eps if not e & 0x80), None)
                path = "/dev/bus/usb/%03d/%03d" % (int(open(d + "/busnum").read()), int(open(d + "/devnum").read()))
                return path, int(open(i + "/bInterfaceNumber").read(), 16), ein, eout
        except (OSError, StopIteration):
            continue
    sys.exit("找不到廠商號 303a 的設備端板子（或它現在的 USB 模式沒有這個接口）")


class Interface:
    """with Interface(cls) as (fd, ifno, ein, eout)：從內核驅動手裡拿走接口，退出時還回去"""

    def __init__(self, cls):
        self.path, self.ifno, self.ein, self.eout = find(cls)

    def __enter__(self):
        self.fd = os.open(self.path, os.O_RDWR)
        try:
            ioctl_ptr(self.fd, USBDEVFS_IOCTL, struct.pack("iiP", self.ifno, USBDEVFS_DISCONNECT, 0))
        except OSError:
            pass                                            # 沒有驅動綁著：不用放
        fcntl.ioctl(self.fd, USBDEVFS_CLAIMINTERFACE, struct.pack("I", self.ifno))
        return self.fd, self.ifno, self.ein, self.eout

    def __exit__(self, *exc):
        try:
            fcntl.ioctl(self.fd, USBDEVFS_RELEASEINTERFACE, struct.pack("I", self.ifno))
            ioctl_ptr(self.fd, USBDEVFS_IOCTL, struct.pack("iiP", self.ifno, USBDEVFS_CONNECT, 0))
        except OSError:
            pass
        os.close(self.fd)


def bulk(fd, ep, data_or_len, timeout_ms=3000):
    """一次批量/中斷傳輸。data_or_len 是 bytes（發）或長度（收）。返回收到的數據"""
    n = data_or_len if isinstance(data_or_len, int) else len(data_or_len)
    buf = C.create_string_buffer(n) if isinstance(data_or_len, int) else C.create_string_buffer(bytes(data_or_len), n)
    r = ioctl_ptr(fd, USBDEVFS_BULK, struct.pack("IIIxxxxP", ep, n, timeout_ms, C.addressof(buf)))
    return buf.raw[:r]


def scsi(cdb, dir_in, data=b"", length=0):
    """Bulk-Only 協議發一條 SCSI 命令：返回 (CSW 狀態, 收到的數據)。狀態 0 = 成功"""
    with Interface("08") as (fd, ifno, ein, eout):
        # 先做協議規定的復位恢復（類請求 0xFF + 清兩個端點的 halt）：上一個用這個接口的程序可能把它留在錯誤狀態
        ioctl_ptr(fd, USBDEVFS_CONTROL, struct.pack("BBHHHIxxxxP", 0x21, 0xFF, 0, ifno, 0, 1000, 0))
        for ep in (ein, eout):
            fcntl.ioctl(fd, USBDEVFS_CLEAR_HALT, struct.pack("I", ep))
        n = length if dir_in else len(data)
        cbw = struct.pack("<4sIIBBB16s", b"USBC", 0x5254, n, 0x80 if dir_in else 0, 0, len(cdb), bytes(cdb).ljust(16, b"\0"))
        bulk(fd, eout, cbw)
        got = b""
        if n and dir_in:
            got = bulk(fd, ein, n)
        elif n:
            bulk(fd, eout, data)
        try:
            csw = bulk(fd, ein, 13)
        except BrokenPipeError:             # 數據比申請的短時設備 STALL IN 端點：清掉再讀 CSW
            fcntl.ioctl(fd, USBDEVFS_CLEAR_HALT, struct.pack("I", ein))
            csw = bulk(fd, ein, 13)
        return struct.unpack("<4sIIB", csw)[3], got


def vendor(tag, dir_in, data=b"", length=0):
    """廠商命令 FB + 四個字母，長度放在 CDB 第 7、8 字節（同 s1tool.c）"""
    n = length if dir_in else len(data)
    return scsi(bytes([0xFB]) + tag + bytes([0, 0, n >> 8, n & 255, 0]), dir_in, data, length)


def keys(seconds):
    """讀鍵盤接口的中斷端點，返回按下的鍵（只看每份報告的第一個鍵）"""
    out = []
    with Interface("03") as (fd, ifno, ein, eout):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                r = bulk(fd, ein, 8, 200)
            except TimeoutError:
                continue
            if len(r) == 8 and r[2]:
                out.append("%02x%s" % (r[2], "S" if r[0] & 0x22 else ""))
    return out


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "msg":
        st, _ = vendor(b"TMSG", False, a[2].encode()[:120])
    elif len(a) == 3 and a[0] == "next":
        st, _ = vendor(b"NEXT", False, a[2].encode()[:39])
    elif len(a) == 2 and a[0] in ("get", "getcfg"):
        st, d = vendor(b"TGET" if a[0] == "get" else b"GCFG", True, length=1024)
        text = d.split(b"\0")[0].decode(errors="replace")
        if st == 0 and a[0] == "get" and not text:
            return 3
        if text:
            print(text)
    elif len(a) == 2 and a[0] == "keys":
        print(" ".join(keys(float(a[1]))))
        return 0
    else:
        print(__doc__.strip(), file=sys.stderr)
        return 1
    return 0 if st == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
