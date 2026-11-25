#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * USB 網卡（CDC-NCM）↔ WiFi 二層轉發，USB 模式 net 時啟用（描述符在 kvm_hid.c）。
 * 目標機經這塊板子的 WiFi 上網，拿到的是 WiFi 那個網絡裡的地址。
 */
void kvm_net_init(void);        /* 另外註冊 wifi、net 兩條命令 */
/* 開始（異步）：連 WiFi，連上後目標機那邊「網線插上」。事件：* net link=up / * net err … / * net ip=…
 * 固件裡沒有 WiFi 憑據時返回 ESP_ERR_NOT_SUPPORTED */
esp_err_t kvm_net_start(void);
void kvm_net_stop(void);
bool kvm_net_active(void);
/* net=off|connecting|up|failed link= wifi_rssi= ip=（目標機 DHCP 拿到的地址）to_usb_kb= to_wifi_kb= drop_q= drop_tx= */
void kvm_net_info(char *buf, size_t len);
