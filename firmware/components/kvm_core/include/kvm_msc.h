#pragma once
#include <stddef.h>
#include <stdint.h>

/* 只讀啟動盤（flash 的 bootimg 分區）。USB 描述符和模式切換在 kvm_hid.c */

void kvm_msc_init(void);
/* 重新識別分區裡的鏡像（改寫分區之後調），清統計。返回扇區數，0 = 沒有鏡像 */
uint32_t kvm_msc_rescan(void);
/* USB 重新枚舉（目標機重啟 / 模式切換）時調：被主機彈出的介質放回去 */
void kvm_msc_bus_reset(void);
/* 狀態摘要：msc_blocks= msc_eject= msc_rd_kb= msc_rd_n= msc_rd_span_ms= msc_rd_maxms= msc_err= */
void kvm_msc_info(char *buf, size_t len);
