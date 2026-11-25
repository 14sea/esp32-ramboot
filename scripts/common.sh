# 給 flash_*.sh 用：讀 boards.env、載入 ESP-IDF 環境、按序列號找串口。用 . 引入，不單獨運行
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/boards.env" ] || { echo "沒有 $ROOT/boards.env：照 boards.env.example 寫一份"; exit 1; }
# shellcheck disable=SC1091
. "$ROOT/boards.env"
IMG="$ROOT/boot/out/stage1.img"
IMG_OFFSET=0x310000                      # firmware/device/partitions.csv 的 bootimg

load_idf() {
    # idf.py 要用 IDF 自己的 Python 環境；先把別的 venv 從 PATH 裡拿掉
    if [ -n "${VIRTUAL_ENV:-}" ]; then
        PATH="$(echo "$PATH" | tr ':' '\n' | grep -vxF "$VIRTUAL_ENV/bin" | paste -sd:)"; export PATH; unset VIRTUAL_ENV
    fi
    # shellcheck disable=SC1091
    . "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null
}

# 本機 /dev/serial/by-id 裡序列號（= MAC）對得上的口。兩塊板子外觀一樣，按序列號認，別燒錯
local_port() {
    local mac="$1" p
    for p in /dev/serial/by-id/*; do
        [ -e "$p" ] || continue
        case "$p" in *"${mac}"*|*"${mac//:/_}"*) echo "$p"; return 0 ;; esac
    done
    return 1
}
