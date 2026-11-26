#pragma once
#include <stddef.h>
#include <stdint.h>

/* 目標機 ↔ 設備端的帶內通道（啟動盤上的廠商 SCSI 命令），說明見 kvm_target.c */

/* 註冊 target、cfg 兩條命令，讀出存著的配置。要在 kvm_cmd_init() 之後、鏈路起來之前調 */
void kvm_target_init(void);

/* 目標機發來一條消息（USB 任務裡調，不阻塞）：過濾成可打印 ASCII、截短、記下並發事件 "* target …" */
void kvm_target_msg(const uint8_t *data, size_t len);

/* 把配置文本（key=value\n …）拷進 buf，返回字節數，不帶結尾的 \0 */
size_t kvm_target_cfg(char *buf, size_t max);

/* 主機 → 目標機的隊列（主機用 tq 命令排，目標機用 SCSI 廠商命令 TGET 取）：取出最早的一條拷進 buf，
 * 返回字節數（不帶 \0）；隊列空時返回 0。USB 任務裡調，不阻塞 */
size_t kvm_target_tq_pop(char *buf, size_t max);
