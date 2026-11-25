#!/usr/bin/env python3
"""
把一個串口轉成本機 TCP 端口（只監聽 127.0.0.1），給「橋插在別的機器上」的情況用：

  遠端（橋插著的機器）：python3 serial_relay.py <串口> [端口=5555]
  本機：ssh -N -L 15555:127.0.0.1:5555 user@遠端      然後  kvm.py --port socket://127.0.0.1:15555 …

只轉發字節，不看內容；kvm_secrets（token）不需要放到遠端。一次只服務一個客戶端；
串口暫時消失（橋被重燒、重新枚舉）會自動重開。只依賴 pyserial。
"""
import select
import socket
import sys
import time

import serial


def open_serial(path):
    """原生 Linux 上按默認方式打開 USB-Serial-JTAG 口會經 DTR/RTS 把芯片復位（WSL 經 usbip 時不會）。
    所以：打開前就把兩根線設成不拉，並且整個運行期間只開一次、不隨客戶端連接重開。"""
    while True:
        try:
            ser = serial.Serial()
            ser.port = path
            ser.baudrate = 115200
            ser.timeout = 0
            ser.dtr = False
            ser.rts = False
            ser.open()
            return ser
        except (serial.SerialException, OSError):
            time.sleep(0.5)


def main():
    path = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5555
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print(f"READY {path} -> 127.0.0.1:{port}", flush=True)
    ser = open_serial(path)
    while True:
        cli, _ = srv.accept()
        cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            ser.reset_input_buffer()            # 上一個客戶端走後積下的事件不留給下一個
        except (serial.SerialException, OSError):
            ser = open_serial(path)
        while True:
            try:
                r, _, _ = select.select([cli, ser], [], [], 1.0)
                if cli in r:
                    d = cli.recv(4096)
                    if not d:
                        break
                    ser.write(d)
                if ser in r:
                    d = ser.read(4096)
                    if d:
                        cli.sendall(d)
            except (serial.SerialException, OSError, ValueError) as e:
                if isinstance(e, (ConnectionError, BrokenPipeError)):
                    break
                # 串口沒了（橋被重燒、重新枚舉）：重開，客戶端連接保留
                try:
                    ser.close()
                except OSError:
                    pass
                ser = open_serial(path)
        cli.close()

if __name__ == "__main__":
    main()
