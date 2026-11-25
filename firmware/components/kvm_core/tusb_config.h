#pragma once
/* TinyUSB 配置：兩個 HID 接口（Boot 鍵盤 + 絕對座標滑鼠）+ 只讀啟動盤 + USB 網卡，按模式取捨，跑在 S3 的 OTG 全速口上（GPIO19/20） */

#define CFG_TUSB_MCU                OPT_MCU_ESP32S3
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_DEBUG              0
#define CFG_TUD_DWC2_SLAVE_ENABLE   1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#define CFG_TUSB_MEM_ALIGN          TU_ATTR_ALIGNED(4)

#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 1
#define CFG_TUD_HID                 2
#define CFG_TUD_MIDI                0
#define CFG_TUD_ECM_RNDIS            0
#define CFG_TUD_NCM                 1
/* 每個 NTB 3200 字節裝得下兩個滿幀；多備幾個緩衝，WiFi 來包快於 USB 全速口時少丟 */
#define CFG_TUD_NCM_IN_NTB_N        3
#define CFG_TUD_NCM_OUT_NTB_N       2
#define CFG_TUD_VENDOR              0

#define CFG_TUD_HID_EP_BUFSIZE      16
/* 啟動盤單次傳輸緩衝，不超過 4 KB：再大時 S3 的 OTG 全速口上實測會丟數據 */
#define CFG_TUD_MSC_EP_BUFSIZE      4096
