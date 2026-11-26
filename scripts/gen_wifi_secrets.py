#!/usr/bin/env python3
"""
從 Wifi.txt 生成 firmware/components/kvm_common/include/wifi_secrets.h（USB 模式 net 用的 WiFi 憑據）。
用法：python3 scripts/gen_wifi_secrets.py [Wifi.txt 的路徑，默認是項目根目錄下的]

Wifi.txt 格式（兩行）：
    SSID: 你的網絡名
    Password: 你的密碼

生成的頭文件和 Wifi.txt 都在 .gitignore 裡，不會進版本庫。
本腳本不會把憑據打印到終端。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "Wifi.txt"
DST = ROOT / "firmware/components/kvm_common/include/wifi_secrets.h"


def strip_quotes(s: str) -> str:
    """Wifi.txt 裡的值可能帶成對的引號，去掉最外層一對。"""
    s = s.strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "\"'":
        s = s[1:-1]
    return s


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def parse(path) -> dict:
    """讀 Wifi.txt，返回 {ssid, password, eap_ssid, eap_id, eap_pw}（沒有的是空字符串）。tools/kvm.py wifi load 也用它。"""
    text = pathlib.Path(path).read_text(encoding="utf-8")
    fields = {}
    for line in text.splitlines():
        m = re.match(r"^\s*([^:=]+)\s*[:=]\s*(.*?)\s*$", line)
        if m:
            fields[m.group(1).strip().lower()] = m.group(2)

    # 可選的企業網絡段（WPA2-Enterprise，如 eduroam）：一行只有網絡名，後面兩行依次是帳號、密碼（可帶引號），例：
    #     eduroam
    #     "user@example.edu"
    #     "密碼"
    # 寫成 "EAP_SSID: … / EAP_Identity: … / EAP_Password: …" 也可以。固件裡用 "wifi eap" 命令切過去
    eap_ssid = strip_quotes(fields.get("eap_ssid", ""))
    eap_id = strip_quotes(fields.get("eap_identity", ""))
    eap_pw = strip_quotes(fields.get("eap_password", ""))
    bare = [strip_quotes(l.strip()) for l in text.splitlines()
            if l.strip() and not re.match(r"^\s*(ssid|password|eap_\w+)\s*[:=]", l, re.I)]
    if not eap_ssid and len(bare) >= 3:
        eap_ssid, eap_id, eap_pw = bare[0], bare[1], bare[2]
    return {"ssid": strip_quotes(fields.get("ssid", "")), "password": strip_quotes(fields.get("password", "")),
            "eap_ssid": eap_ssid, "eap_id": eap_id, "eap_pw": eap_pw}


def main() -> int:
    if not SRC.exists():
        print(f"找不到 {SRC}", file=sys.stderr)
        return 1

    w = parse(SRC)
    ssid, password = w["ssid"], w["password"]
    eap_ssid, eap_id, eap_pw = w["eap_ssid"], w["eap_id"], w["eap_pw"]
    anon_id = "anonymous@" + eap_id.split("@", 1)[1] if "@" in eap_id else "anonymous"

    if not ssid:
        print("Wifi.txt 裡沒讀到 SSID", file=sys.stderr)
        return 1

    DST.parent.mkdir(parents=True, exist_ok=True)
    DST.write_text(
        "/* 由 scripts/gen_wifi_secrets.py 從 Wifi.txt 生成，請勿手工編輯，勿提交 */\n"
        "#pragma once\n\n"
        f'#define WIFI_SSID     "{c_escape(ssid)}"\n'
        f'#define WIFI_PASSWORD "{c_escape(password)}"\n'
        + (
            "\n/* 企業網絡（PEAP/TTLS + MSCHAPv2），固件裡可切換 */\n"
            f'#define WIFI_EAP_SSID     "{c_escape(eap_ssid)}"\n'
            f'#define WIFI_EAP_IDENTITY "{c_escape(eap_id)}"\n'
            # 外層身分在關聯時明文發送，用匿名的；真實帳號只在加密隧道裡當內層用戶名
            f'#define WIFI_EAP_ANON_ID  "{c_escape(anon_id)}"\n'
            f'#define WIFI_EAP_PASSWORD "{c_escape(eap_pw)}"\n'
            if eap_ssid else ""
        ),
        encoding="utf-8",
    )
    print(f"已生成 {DST.relative_to(ROOT)}  (SSID {len(ssid)} 字符, 密碼 {len(password)} 字符"
          + (f"；企業網絡 SSID {len(eap_ssid)} 字符, 帳號 {len(eap_id)} 字符, 密碼 {len(eap_pw)} 字符" if eap_ssid else "") + ")")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
