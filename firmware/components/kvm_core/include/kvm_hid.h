#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    KVM_USB_DETACHED,   /* 沒接、或目標電腦沒開機（VBUS 已切斷，S3 分不清這兩種） */
    KVM_USB_MOUNTED,
    KVM_USB_SUSPENDED,  /* 目標睡眠；按鍵時會嘗試遠程喚醒 */
} kvm_usb_state_t;

/* USB 身分：見 kvm_hid.c 開頭 */
/* 數值存在 NVS 裡（鍵 "usbmode"），不要改順序 */
typedef enum { KVM_USBMODE_HID, KVM_USBMODE_HID_MSC, KVM_USBMODE_MSC, KVM_USBMODE_NET, KVM_USBMODE_COUNT } kvm_usb_mode_t;

esp_err_t kvm_hid_start(void);

/* 開機以來的總線事件計數：usb_rst= usb_mnt= usb_setup= usb_susp= usb_resume= */
void kvm_hid_usb_counters(char *buf, size_t len);

kvm_usb_mode_t kvm_hid_mode(void);
const char *kvm_hid_mode_name(void);
/* 切換 USB 身分（"hid" | "hid+msc" | "msc" | "net"），阻塞約 0.6 秒，目標機看到的是拔掉再插上。
 * save = 存進 NVS 作為開機模式。未知名字返回 ESP_ERR_NOT_FOUND */
esp_err_t kvm_hid_set_mode(const char *name, bool save);

kvm_usb_state_t kvm_hid_usb_state(void);
uint8_t kvm_hid_leds(void);             /* 主機回送的鍵盤燈：bit0 Num, bit1 Caps, bit2 Scroll, bit3 Compose, bit4 Kana */
bool kvm_hid_boot_protocol(void);       /* true = 主機切到了 Boot Protocol（BIOS 通常會） */

/* 發一份鍵盤報告：mod 修飾鍵位圖 + 至多一個鍵（0 = 無）。會等端點就緒、處理遠程喚醒 */
esp_err_t kvm_hid_send(uint8_t mod, uint8_t key);
/* 絕對座標滑鼠：x、y 是 HID 座標 0–32767（映射到目標整個桌面；照片座標怎麼換算是 agent 的事）。
 * buttons：bit0 左、bit1 右、bit2 中；wheel：滾輪步數（正 = 向上） */
esp_err_t kvm_hid_mouse(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel);
void kvm_hid_mouse_pos(uint16_t *x, uint16_t *y);

/* 放開所有按鍵和滑鼠按鈕，失聯自保用，盡力而為 */
void kvm_hid_release_all(void);

/* 300ms 後斷開 USB、切回 USB-Serial-JTAG、重啟進 ROM 下載模式（目標電腦上出現 303a:1001 供 esptool 燒錄） */
void kvm_hid_reboot_to_download(void);
