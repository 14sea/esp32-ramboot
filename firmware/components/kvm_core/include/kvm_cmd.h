#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define KVM_FW_VERSION "0.10"

void kvm_cmd_init(void);

/* 鏈路收到完整一行（不含 \n）。在 NimBLE 任務裡調用，只拷貝入隊，不執行 */
void kvm_cmd_post_line(const char *line);

/* 異步事件，發給主機為 "* <文本>"。任何任務都可調用，不阻塞 */
void kvm_cmd_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void kvm_cmd_link_up(void);
void kvm_cmd_link_down(void);

/*
 * ---- 給引用本組件的工程加命令用 ----
 *
 * 註冊都要在 kvm_cmd_init() 之後、kvm_ble_start() 之前做完：命令表之後只讀，沒有鎖。
 * 名字和 caps 字符串的指針要一直有效（用字符串常量）。
 */

/* 帶序號命令的處理函數，在命令任務裡調用，不能長時間阻塞。cmd = 命令名，args = 其餘部分（可改寫）。
 * 每條命令必須恰好發一次最終回執：kvm_cmd_reply()，或者交給 kvm_cmd_start_job() 的任務去發 */
typedef void (*kvm_cmd_fn_t)(uint32_t seq, const char *cmd, char *args);
esp_err_t kvm_cmd_register(const char *name, kvm_cmd_fn_t fn);

/* 最終回執 "<seq> <文本>"，文本以 "ok" 或 "err " 開頭。會緩存起來供主機重發同一序號時再發 */
void kvm_cmd_reply(uint32_t seq, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* 回執之前的數據：原樣發到鏈路上，調用方自己組行（"| " 開頭、"\n" 結尾）。鏈路不通返回 false */
bool kvm_cmd_send(const char *data, size_t len);

/*
 * 長任務：放進執行任務裡跑，期間命令任務照常處理心跳和 abort。同一時間只有一個任務，
 * 已有任務在跑時返回 ESP_ERR_INVALID_STATE。任務函數要自己發最終回執，並定期看 kvm_cmd_aborted()。
 */
typedef void (*kvm_job_fn_t)(uint32_t seq, void *arg);
esp_err_t kvm_cmd_start_job(uint32_t seq, kvm_job_fn_t fn, void *arg);
bool kvm_cmd_busy(void);
bool kvm_cmd_aborted(void);
const char *kvm_cmd_abort_reason(void);
void kvm_cmd_abort(const char *reason);         /* 任務自己發現做不下去（例如鏈路發送失敗）時調 */

/* state：往 state 回執後面接 "k=v …"（可為 NULL）；caps：接在 caps 回執後面的固定文本（可為 NULL） */
typedef void (*kvm_info_fn_t)(char *buf, size_t len);
esp_err_t kvm_cmd_register_info(kvm_info_fn_t state, const char *caps);
