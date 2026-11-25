#!/bin/sh
# SSH 進 QEMU 裡內存啟動的 Ubuntu（端口 2222 轉發）
exec ssh -i "$(dirname "$0")/keys/id_ed25519" -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -o LogLevel=ERROR ubuntu@127.0.0.1 "$@"
