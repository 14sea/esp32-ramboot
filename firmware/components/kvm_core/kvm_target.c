/*
 * 目標機 ↔ 設備端的帶內通道，走啟動盤上的廠商 SCSI 命令（操作碼 0xFB + 四個字母，見 kvm_msc.c）：
 *
 *   TMSG（數據出）  目標機裡的系統報進度：Stage-1 的每一步、拿到的 IP、失敗原因、進系統後的機器信息。
 *                   設備端轉成事件 "* target <文本>" 發給主機，同時留在環形緩衝裡（主機當時不在線也能事後用 target 命令讀）。
 *   GCFG（數據入）  目標機讀一小段配置（key=value 每行一條），主機經藍牙用 cfg 命令寫、存在 NVS 裡。
 *                   換下載地址、加內核參數不用重做啟動盤鏡像。配置的含義由 Stage-1 解釋，設備端只存不看。
 *
 * 主機看不到目標機的屏幕，這是它知道「目標機走到哪一步了」的唯一途徑。
 * 目標機發來的文本一律當數據：只留可打印 ASCII，截到定長，主機那邊不要把它當命令執行。
 */
#include "kvm_target.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kvm_cmd.h"
#include "nvs.h"

static const char *TAG = "kvm_target";

#define MSG_MAX   120           /* 單條消息最長 */
#define MSG_N     48            /* 環形緩衝條數 */
#define CFG_MAX   1000          /* 配置總長上限（NVS 單個 blob、BLE 單行都放得下） */

static struct { uint32_t t; char text[MSG_MAX + 1]; } s_msgs[MSG_N];
static uint32_t s_msg_total;    /* 開機以來收到的總條數；s_msg_total % MSG_N 是下一條的位置 */
static char s_cfg[CFG_MAX + 1];
static SemaphoreHandle_t s_lock;

/* ---------------- 來自 USB 任務 ---------------- */

void kvm_target_msg(const uint8_t *data, size_t len)
{
    char text[MSG_MAX + 1];
    size_t n = 0;
    for (size_t i = 0; i < len && n < MSG_MAX; i++) {
        uint8_t c = data[i];
        if (c == '\0' || c == '\n') break;
        text[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    text[n] = '\0';
    if (!n) return;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        uint32_t i = s_msg_total % MSG_N;
        s_msgs[i].t = (uint32_t)(esp_timer_get_time() / 1000000);
        strlcpy(s_msgs[i].text, text, sizeof(s_msgs[i].text));
        s_msg_total++;
        xSemaphoreGive(s_lock);
    }
    kvm_cmd_event("target %s", text);
}

size_t kvm_target_cfg(char *buf, size_t max)
{
    size_t n = 0;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        n = strlen(s_cfg);
        if (n > max) n = max;
        memcpy(buf, s_cfg, n);
        xSemaphoreGive(s_lock);
    }
    return n;
}

/* ---------------- 主機命令 ---------------- */

/* target         列出收到的消息（最多最近 MSG_N 條），每行 "| <收到時的開機秒數> <文本>"
 * target clear   清空 */
static void cmd_target(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd;
    if (!strcmp(args, "clear")) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_msg_total = 0;
        xSemaphoreGive(s_lock);
        kvm_cmd_reply(seq, "ok cleared");
        return;
    }
    if (*args) { kvm_cmd_reply(seq, "err usage: target | target clear"); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t total = s_msg_total, n = total < MSG_N ? total : MSG_N;
    xSemaphoreGive(s_lock);
    for (uint32_t k = 0; k < n; k++) {
        char line[MSG_MAX + 24];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t i = (total - n + k) % MSG_N;
        int len = snprintf(line, sizeof(line), "| %" PRIu32 " %s\n", s_msgs[i].t, s_msgs[i].text);
        xSemaphoreGive(s_lock);
        kvm_cmd_send(line, (size_t)len);
    }
    kvm_cmd_reply(seq, "ok n=%" PRIu32 " total=%" PRIu32 " now=%lld", n, total, esp_timer_get_time() / 1000000);
}

static bool key_ok(const char *k, size_t n)
{
    if (!n || n > 24) return false;
    for (size_t i = 0; i < n; i++) if (!(islower((unsigned char)k[i]) || isdigit((unsigned char)k[i]) || k[i] == '_')) return false;
    return true;
}

/* 從 s_cfg 裡刪掉 key 那一行（調用方持鎖） */
static void cfg_del_locked(const char *key, size_t klen)
{
    char *p = s_cfg;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t linelen = e ? (size_t)(e - p) + 1 : strlen(p);
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') memmove(p, p + linelen, strlen(p + linelen) + 1);
        else p += linelen;
    }
}

static esp_err_t cfg_save_locked(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("kvm", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = s_cfg[0] ? nvs_set_blob(h, "tcfg", s_cfg, strlen(s_cfg)) : nvs_erase_key(h, "tcfg");
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* cfg                    列出配置，每行 "| key=value"
 * cfg set key=value      設一項（已有就替換）。key 只能是小寫字母、數字、下劃線；value 是可打印 ASCII，可以帶空格
 * cfg del key            刪一項
 * cfg clear              全清
 * 目標機下一次啟動時讀到新配置（Stage-1 啟動時讀一次） */
static void cmd_cfg(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd;
    esp_err_t e = ESP_OK;
    if (!*args) {
        char copy[CFG_MAX + 1];
        size_t n = kvm_target_cfg(copy, CFG_MAX);
        copy[n] = '\0';
        for (char *p = copy; *p;) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            char line[CFG_MAX + 8];
            int len = snprintf(line, sizeof(line), "| %s\n", p);
            kvm_cmd_send(line, (size_t)len);
            if (!nl) break;
            p = nl + 1;
        }
        kvm_cmd_reply(seq, "ok bytes=%u max=%d", (unsigned)n, CFG_MAX);
        return;
    }
    if (!strcmp(args, "clear")) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg[0] = '\0';
        e = cfg_save_locked();
        xSemaphoreGive(s_lock);
    } else if (!strncmp(args, "del ", 4)) {
        const char *k = args + 4;
        if (!key_ok(k, strlen(k))) { kvm_cmd_reply(seq, "err bad_key"); return; }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cfg_del_locked(k, strlen(k));
        e = cfg_save_locked();
        xSemaphoreGive(s_lock);
    } else if (!strncmp(args, "set ", 4)) {
        const char *kv = args + 4, *eq = strchr(kv, '=');
        if (!eq || !key_ok(kv, (size_t)(eq - kv))) { kvm_cmd_reply(seq, "err bad_key（小寫字母、數字、下劃線，最長 24）"); return; }
        for (const char *p = eq + 1; *p; p++) if (*p < 0x20 || *p >= 0x7f) { kvm_cmd_reply(seq, "err bad_value"); return; }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cfg_del_locked(kv, (size_t)(eq - kv));
        if (strlen(s_cfg) + strlen(kv) + 1 > CFG_MAX) {
            xSemaphoreGive(s_lock);
            kvm_cmd_reply(seq, "err too_long max=%d", CFG_MAX);
            return;
        }
        strlcat(s_cfg, kv, sizeof(s_cfg));
        strlcat(s_cfg, "\n", sizeof(s_cfg));
        e = cfg_save_locked();
        xSemaphoreGive(s_lock);
    } else {
        kvm_cmd_reply(seq, "err usage: cfg | cfg set key=value | cfg del key | cfg clear");
        return;
    }
    if (e != ESP_OK) { kvm_cmd_reply(seq, "err nvs:%s", esp_err_to_name(e)); return; }
    kvm_cmd_reply(seq, "ok bytes=%u", (unsigned)strlen(s_cfg));
}

void kvm_target_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READONLY, &h) == ESP_OK) {
        size_t n = CFG_MAX;
        if (nvs_get_blob(h, "tcfg", s_cfg, &n) == ESP_OK) s_cfg[n] = '\0';
        else s_cfg[0] = '\0';
        nvs_close(h);
    }
    ESP_LOGI(TAG, "目標機配置 %u 字節", (unsigned)strlen(s_cfg));
    kvm_cmd_register("target", cmd_target);
    kvm_cmd_register("cfg", cmd_cfg);
    kvm_cmd_register_info(NULL, "target_msg=1 cfg=1");
}
