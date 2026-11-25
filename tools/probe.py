#!/usr/bin/env python3
"""probe.py <seconds> [key ...]：按鍵（可為空），然後聽 N 秒事件，最後打印 USB/啟動盤計數。"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kvm
k = kvm.Kvm(os.environ['KVM_PORT'])
k.connect(20)
secs = float(sys.argv[1]); keys = sys.argv[2:]
if keys:
    ok, s, _ = k.call('key ' + ' '.join(keys), 10)
    print('key', ' '.join(keys), '->', s.split(' ', 1)[1])
end = time.monotonic() + secs
while time.monotonic() < end:
    k.heartbeat()
    line = k.readline(0.3)
    if line and (line.startswith('*') or line.startswith('@')):
        print(f'  +{secs - (end - time.monotonic()):4.1f}s {line}')
ok, st, _ = k.call('state', 5); ok, md, _ = k.call('mode', 5)
pick = lambda s, pat: ' '.join(re.findall(pat, s))
print(' ', pick(st, r'(?:usb|proto|leds|uptime|boots)=\S+'), '|', pick(md, r'(?:usb_rst|usb_mnt|usb_setup|usb_susp|msc_inq|msc_tur|msc_rd_kb|msc_rd_n|msc_eject|msc_other)=\S+'))
