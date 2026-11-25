#!/usr/bin/env bash
# 生成 BLE 配對口令（6 位數字）和應用層 token，設備固件、橋固件和 tools/kvm.py 共用。
# 已存在則不覆蓋（覆蓋後兩塊板子都要重燒，並在橋上 @unpair）。用法：bash scripts/gen_secrets.sh [--force]
set -euo pipefail
out="$(cd "$(dirname "$0")/.." && pwd)/firmware/components/kvm_common/include/kvm_secrets.h"
if [ -e "$out" ] && [ "${1:-}" != "--force" ]; then echo "已存在：$out（不覆蓋）"; exit 0; fi
passkey=$(( $(od -An -N4 -tu4 /dev/urandom) % 900000 + 100000 ))
token=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
umask 077
cat > "$out" <<H
#pragma once
/* 由 scripts/gen_secrets.sh 生成，不進版本庫，不要提交、不要貼出來 */
#define KVM_PASSKEY $passkey
#define KVM_TOKEN   "$token"
H
echo "已生成：$out（口令 6 位、token ${#token} 字符）"
