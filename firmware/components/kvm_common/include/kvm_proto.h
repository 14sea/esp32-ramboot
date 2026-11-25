#pragma once
/*
 * 設備（firmware/device）↔ 橋（firmware/bridge）/ 主機 的 BLE 協議常量。改這裡兩塊板子都要重燒。
 *
 * GATT 服務 7a4f0001-8c5e-4d2a-9b13-6e0d2c5a9f10，兩個特徵：
 *   7a4f0002  RX：主機 → 設備，write / write-no-rsp，要求加密 + MITM 認證
 *   7a4f0003  TX：設備 → 主機，notify
 * 兩個方向都是「按行」的文本流（\n 結尾），一行可能被拆成多個 BLE 包，接收方自己拼。
 *
 * 行格式：
 *   主機 → 設備：  <seq> <cmd> [參數]        seq=0 表示不參與去重（hello / hb / abort）
 *   設備 → 主機：  <seq> ok [文本] | <seq> err <原因>   每條 seq>0 的命令恰好一條最終回執
 *                  | <文本>                    回執前的多行數據
 *                  * <事件>                    異步事件
 *   橋本地：       @ 開頭的行只在橋和主機之間，不轉發
 */
#include "kvm_secrets.h"

/* 128-bit UUID 的小端字節序（NimBLE BLE_UUID128_INIT 的參數順序），id 是第 3、4 字節 */
#define KVM_UUID128(id) \
    0x10, 0x9f, 0x5a, 0x2c, 0x0d, 0x6e, 0x13, 0x9b, 0x2a, 0x4d, 0x5e, 0x8c, (id), 0x00, 0x4f, 0x7a

#define KVM_UUID_SVC  0x01
#define KVM_UUID_RX   0x02
#define KVM_UUID_TX   0x03

#define KVM_LINE_MAX  1024      /* 單行最長（含 type 的文本） */
#define KVM_HB_TIMEOUT_MS 3000  /* 有動作在執行時，這麼久沒收到任何行就中止並放開所有按鍵 */
