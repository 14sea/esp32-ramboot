#pragma once
#include <stdbool.h>
#include <stddef.h>

void kvm_ble_start(void);

/* 鏈路可用：已加密 + MITM 認證 + 對方已訂閱 TX 通知 */
bool kvm_ble_ready(void);

/* 經 TX 特徵發數據（按 MTU 分包），鏈路不可用時返回 false。多任務調用安全，整段不會被別的發送打斷 */
bool kvm_ble_send(const char *data, size_t len);

/* WiFi 推流期間放寬連接間隔讓出射頻（true），結束後恢復（false） */
void kvm_ble_set_slow(bool slow);
int kvm_ble_itvl_ms(void);      /* 當前連接間隔，沒連著是 0 */

/* true：斷開連接、停止廣播，射頻完全讓給 WiFi（控制通道已換到 ESP-NOW 時用）；false：恢復廣播 */
void kvm_ble_set_quiet(bool quiet);

/* 狀態摘要，給 state 命令用 */
void kvm_ble_info(char *buf, size_t len);
