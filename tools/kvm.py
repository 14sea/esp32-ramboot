#!/usr/bin/env python3
"""
經橋（firmware/bridge，USB 串口）控制設備（firmware/device）。協議見 firmware/components/kvm_common/include/kvm_proto.h。

  kvm.py status                      橋 + 設備狀態
  kvm.py key ctrl+alt+delete         按鍵（多個參數依次按）：kvm.py key down down enter
  kvm.py type 'hello world' [--enter]  打字（美式佈局），--enter 結尾補回車
  kvm.py repeat F12 15000 100        設備本地每 100ms 按一次 F12，持續 15 秒
  kvm.py move X Y | click X Y [left|right|middle] [次數] | scroll N | drag X1 Y1 X2 Y2
                                     絕對座標滑鼠，X Y 是 HID 座標 0–32767
  kvm.py mode [hid|hid+msc|msc|net] [save]   查詢/切換 USB 身分（鍵鼠 / 鍵鼠+啟動盤 / 只有啟動盤 / 鍵盤+啟動盤+USB 網卡）
                                     save = 存為開機模式。net = 目標機經設備的 WiFi 上網（沒有可用網卡的機器用）
  kvm.py mode next <hid|hid+msc|msc> [秒] | mode next off
                                     目標機下次重啟時才換模式（只一次、不存）；給了秒數就在那之後換回來
  kvm.py wifi [home|eap]             選設備用哪套 WiFi 憑據（普通網絡 / 企業網絡如 eduroam），切換後 kvm.py reboot 生效
  kvm.py wifi load [Wifi.txt]        把 Wifi.txt 裡的憑據經藍牙存進設備的 NVS（不用重編固件），之後 kvm.py reboot
  kvm.py wifi set home|eap           同上，憑據從鍵盤輸入（密碼不回顯）；wifi forget home|eap 刪掉
  kvm.py net                         USB 網卡轉發的狀態和計數
  kvm.py run '<命令>' [--timeout 秒]  在目標機上以 root 跑命令、打印輸出、以命令的退出碼退出（要目標機上有 ramboot-agent）
  kvm.py put <本地文件> <目標機路徑> [權限]   把文件傳到目標機（同上，經藍牙 + 啟動盤，約幾 KB/s）
  kvm.py tq [<文本> | clear]          主機 → 目標機隊列（run / put 用它）的狀態，或直接排一條
  kvm.py wait <文字> [秒] [--then "<命令>"]    等含這段文字的事件，等到後可緊接著發一條命令
  kvm.py mtrace [clear]              目標機對啟動盤做了什麼（讀了哪些扇區）：看不到屏幕時判斷 BIOS 在幹什麼
  kvm.py target [clear]              目標機報來的進度和信息（Stage-1 的每一步、IP、機器型號……），主機不在線時收到的也在
  kvm.py cfg [set key=value | del key | clear]   存在設備裡、目標機啟動時讀的配置：
                                     base=<地址> 換下載站；cmdline=<參數> 追加內核參數（ramboot.console=1 = 控制台自動登入）
  kvm.py release | log | caps | state | dfu | reboot
  kvm.py raw '<cmd> [args]'          發任意帶序號命令
  kvm.py bridge @status              橋本地命令

串口：--port 或環境變量 KVM_PORT；橋插在別的機器上時用 socket://127.0.0.1:端口（見 tools/serial_relay.py）。
退出碼：0 ok，1 設備回 err，2 鏈路/超時。
每條命令帶序號，鏈路斷了重連後重發同一序號，設備去重，不會執行兩次。
等回執期間每秒發心跳；設備有動作在跑時 3 秒收不到任何行會自行中止並放開所有按鍵。
"""
import argparse
import base64
import getpass
import hashlib
import importlib.util
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
        self.on_event = None        # 收到 "* …" 事件時調用；None = 打到 stderr

    # ---- 行 I/O ----
    def send(self, line):
        if self.verbose:
            shown = re.sub(r"^(\d+ wifi set \w+) .*", r"\1 <憑據不顯示>", line)
            print(f">> {shown}", file=sys.stderr)
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
            elif s.startswith("* ") and self.on_event:
                self.on_event(s)
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


# ---- 主機 → 目標機通道（板子的 tq 隊列 + 目標機上的 ramboot-agent，見 boot/stage2/seed.in/…/ramboot-agent）----
# 排給 agent 的消息：run <id> <命令> | put <id> <權限> <路徑> | dat <id> <base64> | end <id> <sha256>
# agent 的回報（經 TMSG，主機收到事件 "* target os <id>…"）：<id>| <輸出行> | <id> rc=<n> | <id> put ok <字節> | <id> put err <原因>

def tq(k, text):
    """排一條；隊列滿就等 agent 取走再發"""
    end = time.monotonic() + 60
    while True:
        ok, s, _ = k.call("tq " + text, 10)
        if ok:
            return
        if "err full" not in s or time.monotonic() > end:
            raise LinkError(f"tq：{s}")
        time.sleep(0.3)


def agent_wait(k, rid, timeout, on_line, early=()):
    """等 agent 對 rid 的最終回報，返回那一行（去掉 "<id> " 前綴）。期間的輸出行交給 on_line。
    early：發 tq 期間（回執之前）就收到的事件，先處理。
    藍牙斷開（@down）時：等重連、重新 hello，再從板子的 target 環形緩衝（最近 48 條）補回斷開期間漏掉的行和最終回報。
    輸出比環形緩衝長、對不上時，提示可能缺了輸出"""
    pre = f"* target os {rid}"
    final, seen = [], []

    def handle(s):
        if s.startswith(pre + "| "):
            seen.append(s[len(pre) + 2:])
            on_line(seen[-1])
        elif s.startswith(pre + " "):
            final.append(s[len(pre) + 1:])
        else:
            print(s, file=sys.stderr)

    def recover(why):
        print(f"{why}：鏈路斷開，等重連後從板子補回漏掉的輸出", file=sys.stderr)
        # 重連和讀緩衝期間到的事件都已經在環形緩衝裡（板子先記緩衝、再發事件，事件和 target 命令同一個隊列）：
        # 以緩衝為準，這期間的 target 事件不再單獨處理
        k.on_event = lambda s: None if s.startswith("* target ") else print(s, file=sys.stderr)
        k.wait_up(60)
        k.hello()
        _, _, rows = k.call("target", 15)
        k.on_event = handle
        texts = [r.split(" ", 1)[1] for r in rows if " " in r]         # 每行 "<開機秒數> <文本>"
        out = [t[len(rid) + 5:] for t in texts if t.startswith(f"os {rid}| ")]
        if out[:len(seen)] == seen:
            for line in out[len(seen):]:
                seen.append(line)
                on_line(line)
        else:
            print("（鏈路斷開期間的輸出可能不全：板子只留最近 48 條）", file=sys.stderr)
        ends = [t[len(rid) + 4:] for t in texts if t.startswith(f"os {rid} ")]
        if ends and not final:
            final.append(ends[-1])

    for s in early:
        handle(s)
    k.on_event = handle
    end = time.monotonic() + timeout
    while not final and time.monotonic() < end:
        k.heartbeat()
        s = k.readline(0.3)
        if not s:
            continue
        if s.startswith("* "):
            handle(s)
        elif s.startswith("@down"):
            recover(s)
    k.on_event = None
    return final[0] if final else None


def do_run(k, cmdline, timeout):
    if len(cmdline) > 900:
        sys.exit("命令太長（最多 900 字符）：長腳本先用 put 傳過去再 run")
    rid = secrets.token_hex(2)
    early = []
    k.on_event = early.append       # agent 可能在 tq 的回執之前就開始回報：先存著，交給 agent_wait
    tq(k, f"run {rid} {cmdline}")
    r = agent_wait(k, rid, timeout, lambda l: print(l, flush=True), early)
    if r is None:
        print(f"{timeout:.0f} 秒內沒有回報（目標機上沒跑 ramboot-agent？命令還沒結束？）", file=sys.stderr)
        return 2
    m = re.match(r"rc=(\d+)", r)
    return int(m.group(1)) if m else 1


def do_put(k, local, remote, mode, timeout):
    data = open(local, "rb").read()
    if not remote.startswith("/") or " " in remote:
        sys.exit("目標機路徑要是不帶空格的絕對路徑")
    rid = secrets.token_hex(2)
    t0 = time.monotonic()
    early = []
    k.on_event = early.append
    tq(k, f"put {rid} {mode} {remote}")
    for i in range(0, len(data), 600):              # 600 字節 → 800 字符的 base64，加前綴後在單行 1024 以內
        tq(k, f"dat {rid} {base64.b64encode(data[i:i + 600]).decode()}")
    tq(k, f"end {rid} {hashlib.sha256(data).hexdigest()}")
    r = agent_wait(k, rid, timeout, lambda l: print(l, flush=True), early)
    if r is None:
        print("沒有回報（目標機上沒跑 ramboot-agent？）", file=sys.stderr)
        return 2
    dt = time.monotonic() - t0
    print(f"{r}（{len(data)} 字節，{dt:.1f} 秒，{len(data) / 1024 / dt:.1f} KB/s）")
    return 0 if r.startswith("put ok") else 1


def wifi_cmds(a):
    """wifi load / set / forget 要發的命令（憑據按十六進制發，空格和引號都不用轉義）。返回 [(命令, 說明)]"""
    hx = lambda s: s.encode("utf-8").hex()
    sub = a.args[0]
    if sub == "forget" and len(a.args) == 2 and a.args[1] in ("home", "eap"):
        return [(f"wifi forget {a.args[1]}", "")]
    if sub == "load":
        path = a.args[1] if len(a.args) > 1 else os.path.join(ROOT, "Wifi.txt")
        spec = importlib.util.spec_from_file_location("gen_wifi_secrets", os.path.join(ROOT, "scripts/gen_wifi_secrets.py"))
        g = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(g)
        w = g.parse(path)
        out = []
        if w["ssid"]:
            out.append((f"wifi set home {hx(w['ssid'])} {hx(w['password'])}", "home"))
        if w["eap_ssid"]:
            out.append((f"wifi set eap {hx(w['eap_ssid'])} {hx(w['eap_id'])} {hx(w['eap_pw'])}", "eap"))
        if not out:
            sys.exit(f"{path} 裡沒讀到憑據")
        return out
    if sub == "set" and len(a.args) > 1 and a.args[1] in ("home", "eap"):
        ssid = input("SSID: ")
        if a.args[1] == "home":
            return [(f"wifi set home {hx(ssid)} {hx(getpass.getpass('密碼: '))}", "home")]
        ident = input("帳號: ")
        return [(f"wifi set eap {hx(ssid)} {hx(ident)} {hx(getpass.getpass('密碼: '))}", "eap")]
    sys.exit("用法：kvm.py wifi load [Wifi.txt] | wifi set home|eap | wifi forget home|eap")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("KVM_PORT", "/dev/ttyACM0"))
    ap.add_argument("--wait", type=float, default=15, help="等設備連上的秒數")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--enter", action="store_true", help="type 結尾補回車")
    ap.add_argument("--then", default="", help="wait 等到事件後緊接著發的命令")
    ap.add_argument("--timeout", type=float, default=120, help="run / put 等目標機回報的秒數")
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
        if a.cmd == "run":
            return do_run(k, " ".join(a.args), a.timeout)
        if a.cmd == "put":
            if len(a.args) not in (2, 3):
                ap.error("用法：put <本地文件> <目標機路徑> [權限，默認 644]")
            return do_put(k, a.args[0], a.args[1], a.args[2] if len(a.args) == 3 else "644", a.timeout)
        if a.cmd == "wifi" and a.args and a.args[0] in ("load", "set", "forget"):
            rc = 0
            for cmd, what in wifi_cmds(a):
                ok, s, _ = k.call(cmd, 10)
                print(s)
                rc |= 0 if ok else 1
            return rc

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
        elif a.cmd in ("target", "cfg", "mtrace", "wifi", "net", "tq"):    # target [clear]；cfg [set key=value | del key | clear]；mtrace [clear]
            cmd = (a.cmd + " " + " ".join(a.args)).strip()
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
