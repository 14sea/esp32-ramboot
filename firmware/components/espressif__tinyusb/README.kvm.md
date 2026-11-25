# TinyUSB（項目內副本）

來源：ESP 組件庫 `espressif/tinyusb` 0.21.0~6，只拷了 `src/`、`lib/networking/`、`CMakeLists.txt`、`LICENSE`。

## 相對上游的改動（只有一處）

`src/device/usbd.c`，`TUSB_REQ_SET_CONFIGURATION`：上游「配置號和當前相同就什麼都不做」，改成相同也重新配置
（關所有端點 → 復位各類驅動 → 重新打開）。

原因：USB 規範要求 SET_CONFIGURATION 之後端點數據翻轉位歸 DATA0，主機也這麼假定。主機**不經總線復位**就換驅動時
（QEMU usb-host 直通、USB/IP、驅動重新綁定），上游行為會讓兩邊翻轉位有一半機率不同步，第一個 bulk OUT 包被設備
當成重傳丟掉——啟動盤收不到命令、主機逾時。2026-11-24 實測：`usb_rst` 計數不變、`msc_out_busy=1`、主機 INQUIRY 逾時。

實機上每次枚舉都有總線復位（配置號先歸 0），本來就不受影響；這個補丁是讓設備在沒有復位的交接下也正確。

升級 TinyUSB 時：重新拷貝後把這一處改動補回去。
