#!/usr/bin/env python3
"""
經橋（firmware/bridge，USB 串口）控制設備（firmware/device）。協議見 firmware/components/kvm_common/include/kvm_proto.h。

  kvm.py status                      橋 + 設備狀態
  kvm.py key ctrl+alt+delete         按鍵（多個參數依次按）：kvm.py key down down enter
  kvm.py type 'hello world' [--enter]  打字（美式佈局），--enter 結尾補回車
  kvm.py repeat F12 15000 100        設備本地每 100ms 按一次 F12，持續 15 秒
  kvm.py move X Y | click X Y [left|right|middle] [次數] | scroll N | drag X1 Y1 X2 Y2
                                     絕對座標滑鼠，X Y 是 HID 座標 0–32767
  kvm.py mode [hid|hid+msc|msc] [save]   查詢/切換 USB 身分（鍵鼠 / 鍵鼠+啟動盤 / 只有啟動盤）；save = 存為開機模式
  kvm.py wait <文字> [秒] [--then "<命令>"]    等含這段文字的事件，等到後可緊接著發一條命令
  kvm.py release | log | caps | state | dfu | reboot
  kvm.py raw '<cmd> [args]'          發任意帶序號命令
  kvm.py bridge @status              橋本地命令

串口：--port 或環境變量 KVM_PORT；橋插在別的機器上時用 socket://127.0.0.1:端口（見 tools/serial_relay.py）。
退出碼：0 ok，1 設備回 err，2 鏈路/超時。
每條命令帶序號，鏈路斷了重連後重發同一序號，設備去重，不會執行兩次。
等回執期間每秒發心跳；設備有動作在跑時 3 秒收不到任何行會自行中止並放開所有按鍵。
"""
import argparse
import os
import re
import secrets
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECRETS = os.path.join(ROOT, "firmware/components/kvm_common/include/kvm_secrets.h")


def load_token():
    with open(SECRETS) as f:
        m = re.search(r'#define\s+KVM_TOKEN\s+"([0-9a-f]+)"', f.read())
    if not m:
        sys.exit(f"讀不到 KVM_TOKEN：{SECRETS}（先跑 scripts/gen_secrets.sh）")
    return m.group(1)


class LinkError(Exception):
    pass


class Kvm:
    def __init__(self, port, verbose=False):
        # 默認 DTR/RTS 都置位且之後不再變動：USB-Serial-JTAG 只在特定的 DTR/RTS 組合下復位芯片，
        # 打開時兩根線同時拉高不會觸發（WSL 經 usbip 實測）。原生 Linux 上會復位，要經 tools/serial_relay.py
        # serial_for_url：除了本機串口，也接受 socket://主機:端口（橋插在別的機器上，經 tools/serial_relay.py + SSH 隧道）
        self.ser = serial.serial_for_url(port, baudrate=115200, timeout=0.05)
        self.buf = b""
        self.verbose = verbose
        self.session = secrets.token_hex(6)
        self.seq = 0
        self.token = load_token()
        self.last_hb = 0.0

    # ---- 行 I/O ----
    def send(self, line):
        if self.verbose:
            print(f">> {line}", file=sys.stderr)
        self.ser.write((line + "\n").encode())

    def readline(self, timeout):
        end = time.monotonic() + timeout
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                line, self.buf = self.buf[:i], self.buf[i + 1:]
                s = line.decode(errors="replace").rstrip("\r")
                if self.verbose:
                    print(f"<< {s}", file=sys.stderr)
                return s
            if time.monotonic() >= end:
                return None
            self.buf += self.ser.read(256)

    def heartbeat(self):
        if time.monotonic() - self.last_hb >= 1.0:
            self.send("0 hb")
            self.last_hb = time.monotonic()

    # ---- 橋 ----
    def bridge(self, cmd, expect, timeout=2.0):
        self.send(cmd)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            s = self.readline(end - time.monotonic())
            if s and s.startswith(expect):
                return s
        raise LinkError(f"橋沒有回應 {cmd}（橋沒接上？串口不對？）")

    def wait_up(self, timeout):
        st = self.bridge("@status", "@status")
        if " ready=1" in st:
            return
        print(f"等設備連上（最多 {timeout:.0f}s）...", file=sys.stderr)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            s = self.readline(end - time.monotonic())
            if s and s.startswith("@up"):
                return
        raise LinkError("設備沒連上（沒上電？不在範圍內？）")

    def hello(self):
        self.send(f"0 hello {self.token} {self.session}")
        end = time.monotonic() + 3
        while time.monotonic() < end:
            s = self.readline(end - time.monotonic())
            if s is None:
                break
            if s.startswith("0 ok kvm"):
                return s
            if s.startswith("0 err auth"):
                sys.exit("設備拒絕 token：兩邊的 kvm_secrets.h 不一致？")
            if s.startswith("@down"):
                raise LinkError("hello 時鏈路斷開")
        raise LinkError("hello 沒有回應")

    def connect(self, wait):
        self.wait_up(wait)
        return self.hello()

    # ---- 命令 ----
    def call(self, cmd, timeout, wait_link=30.0):
        """發一條帶序號命令，等最終回執。返回 (ok, 回執行, 數據行列表)"""
        self.seq += 1
        seq = self.seq
        line = f"{seq} {cmd}"
        data = []
        pat = re.compile(rf"^{seq} (ok|err)\b ?(.*)$")
        deadline = time.monotonic() + timeout
        self.send(line)
        sent_at = time.monotonic()
        while time.monotonic() < deadline:
            self.heartbeat()
            s = self.readline(0.2)
            if s is None:
                # 短命令 3 秒沒回執：重發同一序號（設備去重，最多重發回執）
                if time.monotonic() - sent_at > 3 and timeout <= 15:
                    self.send(line)
                    sent_at = time.monotonic()
                continue
            m = pat.match(s)
            if m:
                return m.group(1) == "ok", s, data
            if s.startswith("| "):
                data.append(s[2:])
            elif s.startswith("* ") or s.startswith("@err"):
                print(s, file=sys.stderr)
            elif s.startswith("@down"):
                print(f"{s}：鏈路斷開，等重連後重發 #{seq}", file=sys.stderr)
                self.wait_up(wait_link)
                self.hello()
                self.send(line)
                sent_at = time.monotonic()
        raise LinkError(f"#{seq} {cmd.split()[0]} 等回執超時")


def do_wait(k, a):
    """wait <事件裡的一段文字> [秒數]：等設備發來含這段文字的事件（"* …" 行），期間收到的事件都打出來。
    --then "<命令>" 是等到之後緊接著發的命令（例：wait "msc eject" 300 --then "mode hid"）。"""
    want = a.args[0]
    end = time.monotonic() + (float(a.args[1]) if len(a.args) > 1 else 60)
    while time.monotonic() < end:
        k.heartbeat()
        line = k.readline(0.3)
        if not line or not line.startswith("*"):
            continue
        print(line, flush=True)
        if want in line:
            if a.then:
                ok, s, _ = k.call(a.then, 30)
                print(s, flush=True)
                return 0 if ok else 1
            return 0
    print(f"等不到含「{want}」的事件", file=sys.stderr)
    return 2


def escape_text(t):
    return t.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("KVM_PORT", "/dev/ttyACM0"))
    ap.add_argument("--wait", type=float, default=15, help="等設備連上的秒數")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--enter", action="store_true", help="type 結尾補回車")
    ap.add_argument("--then", default="", help="wait 等到事件後緊接著發的命令")
    a = ap.parse_args()

    k = Kvm(a.port, a.verbose)
    try:
        if a.cmd == "bridge":
            print(k.bridge(a.args[0], "@", 3))
            return 0
        hello = k.connect(a.wait)
        if a.cmd == "status":
            print(k.bridge("@status", "@status"))
            print(hello)
            ok, s, _ = k.call("state", 5)
            print(s)
            return 0 if ok else 1

        if a.cmd == "wait":
            return do_wait(k, a)

        timeout = 10.0
        if a.cmd == "type":
            text = " ".join(a.args) + ("\n" if a.enter else "")
            cmd = "type " + escape_text(text)
            timeout += len(text) * 0.08
        elif a.cmd == "key":
            cmd = "key " + " ".join(a.args)
            timeout += len(a.args) * 0.1
        elif a.cmd == "repeat":
            cmd = "repeat " + " ".join(a.args)
            timeout += int(a.args[1]) / 1000
        elif a.cmd == "raw":
            cmd = " ".join(a.args)
            timeout = 60
        elif a.cmd == "mode":        # mode [hid|hid+msc|msc] [save]：查詢/切換 USB 身分（鍵鼠 / 鍵鼠+啟動盤 / 只有啟動盤）
            cmd = ("mode " + " ".join(a.args)).strip()
        elif a.cmd in ("move", "click", "scroll", "drag"):
            cmd = a.cmd + " " + " ".join(a.args)
            timeout += 3
        elif a.cmd in ("release", "log", "caps", "state", "dfu", "reboot"):
            cmd = a.cmd
        else:
            ap.error(f"未知命令 {a.cmd}")
        ok, s, data = k.call(cmd, timeout)
        for d in data:
            print(d)
        print(s)
        return 0 if ok else 1
    except LinkError as e:
        print(f"錯誤：{e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
