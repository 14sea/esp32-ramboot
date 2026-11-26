/*
 * 命令層：S3 只執行，判斷都在主機那邊的 agent。
 *
 * 兩個任務：
 *   cmd  —— 解析每一行、做認證和序號去重、回答即時命令；同時當看門狗（心跳超時中止動作）
 *   exec —— 執行按鍵類動作（可能持續幾十秒），每一步之間檢查中止標誌
 * 動作放在獨立任務裡，執行期間 cmd 仍能處理 hb / abort。
 *
 * 留在 S3 本地的例外都在這裡：
 *   1. 時序：repeat 由 S3 自己按時連按（POST 時間窗走 BLE 往返趕不上）
 *   2. 失聯自保：BLE 斷開、或有動作在跑時 KVM_HB_TIMEOUT_MS 內沒收到任何行 → 中止、放開所有按鍵
 *   3. 序號去重：重發同一 seq 不會再執行一次，只重發上次的回執
 *
 * 內建命令和外部組件加的命令走同一張表（kvm_cmd_register）。本組件只有鍵盤、滑鼠、啟動盤；
 * 攝像頭、USB 網卡之類由引用本組件的工程自己註冊。
 */
#include "kvm_cmd.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_ble.h"
#include "kvm_hid.h"
#include "kvm_keys.h"
#include "kvm_link.h"
#include "kvm_msc.h"
#include "kvm_net.h"
#include "kvm_proto.h"
#include "logbuf.h"
#include "nvs.h"

static const char *TAG = "kvm_cmd";

/* 按鍵節奏：修飾鍵先按 10ms，鍵按住 20ms，鬆開後停 20ms ≈ 每秒 25 字 */
#define KEY_MOD_MS   10
#define KEY_HOLD_MS  20
#define KEY_GAP_MS   20
#define MAX_CHORDS   32

typedef enum { ITEM_LINE, ITEM_EVENT, ITEM_LINK_UP, ITEM_LINK_DOWN } item_kind_t;
typedef struct {
    item_kind_t kind;
    char *text;
} item_t;

typedef enum { JOB_CHORDS, JOB_TYPE, JOB_REPEAT, JOB_MOUSE, JOB_EXT } job_kind_t;
typedef struct {
    job_kind_t kind;
    uint32_t seq;
    int n;
    kvm_chord_t chords[MAX_CHORDS];     /* JOB_CHORDS；JOB_REPEAT 只用 chords[0] */
    char *text;                         /* JOB_TYPE：已解碼轉義 */
    int total_ms, period_ms;            /* JOB_REPEAT */
    kvm_job_fn_t fn;                    /* JOB_EXT：外部組件的長任務 */
    void *arg;
    char mop;                           /* JOB_MOUSE：'m' move 'c' click 's' scroll 'd' drag */
    int x, y, x2, y2, count;
    uint8_t btn;
} job_t;

static QueueHandle_t s_q, s_jobq;
static volatile int64_t s_last_rx_us;
static volatile bool s_busy, s_abort;
static const char *volatile s_abort_reason = "";

/* 會話與去重。s_reply_lock 保護下面四個 */
static SemaphoreHandle_t s_reply_lock;
static char s_session[40];
static uint32_t s_last_seq;
static bool s_last_done = true;
static char s_last_reply[520];

static bool s_app_authed;       /* 本次 BLE 連接上 hello 過正確 token */
static int s_token_fail;

/* 命令表和信息鉤子。只在啟動階段（鏈路起來之前）寫，之後只讀，所以不加鎖 */
#define MAX_CMDS  32
#define MAX_INFO  4
static struct { const char *name; kvm_cmd_fn_t fn; } s_cmds[MAX_CMDS];
static int s_ncmds;
static struct { kvm_info_fn_t state; const char *caps; } s_info[MAX_INFO];
static int s_ninfo;

/* ---------------- 發送 ---------------- */

static void send_raw(const char *s)
{
    kvm_link_send(s, strlen(s));
}

static void send_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_fmt(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    kvm_link_send(buf, n);
}

/* 發 seq 的最終回執，並緩存起來供重發 */
static void finish(uint32_t seq, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish(uint32_t seq, const char *fmt, ...)
{
    char body[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[520];
    snprintf(line, sizeof(line), "%" PRIu32 " %s\n", seq, body);
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    if (seq == s_last_seq) {
        strlcpy(s_last_reply, line, sizeof(s_last_reply));
        s_last_done = true;
    }
    xSemaphoreGive(s_reply_lock);
    send_raw(line);
}

/* ---------------- 入隊（給其他任務調用） ---------------- */

static void post(item_kind_t kind, char *text)
{
    item_t it = { .kind = kind, .text = text };
    if (xQueueSend(s_q, &it, 0) != pdTRUE) free(text);
}

void kvm_cmd_post_line(const char *line)
{
    s_last_rx_us = esp_timer_get_time();    /* 收到任何行都算心跳，即使隊列忙 */
    post(ITEM_LINE, strdup(line));
}

void kvm_cmd_event(const char *fmt, ...)
{
    char buf[160] = "* ";
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + 2, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    strlcat(buf, "\n", sizeof(buf));
    if (s_q) post(ITEM_EVENT, strdup(buf));
}

static void request_abort(const char *reason)
{
    if (s_busy && !s_abort) {
        s_abort_reason = reason;
        s_abort = true;
        ESP_LOGW(TAG, "中止當前動作：%s", reason);
    }
}

void kvm_cmd_link_up(void) { post(ITEM_LINK_UP, NULL); }

void kvm_cmd_link_down(void)
{
    request_abort("link_down");     /* 立刻置位，不等 cmd 任務 */
    post(ITEM_LINK_DOWN, NULL);
}

/* ---------------- 執行器 ---------------- */

static bool sleep_ms(int ms)
{
    while (ms > 0) {
        if (s_abort) return false;
        int step = ms > 10 ? 10 : ms;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    return !s_abort;
}

static esp_err_t tap(kvm_chord_t c)
{
    esp_err_t e;
    if (c.mod && c.key) {               /* 修飾鍵先單獨按下，有些 BIOS/系統同一份報告裡的 Shift 會漏 */
        if ((e = kvm_hid_send(c.mod, 0)) != ESP_OK) return e;
        if (!sleep_ms(KEY_MOD_MS)) return ESP_OK;
    }
    if ((e = kvm_hid_send(c.mod, c.key)) != ESP_OK) return e;
    if (!sleep_ms(KEY_HOLD_MS)) return ESP_OK;      /* 中止時由調用方統一放開 */
    if ((e = kvm_hid_send(0, 0)) != ESP_OK) return e;
    sleep_ms(KEY_GAP_MS);
    return ESP_OK;
}

static esp_err_t run_job(const job_t *j)
{
    esp_err_t e = ESP_OK;
    switch (j->kind) {
    case JOB_CHORDS:
        for (int i = 0; i < j->n && e == ESP_OK && !s_abort; i++) e = tap(j->chords[i]);
        break;
    case JOB_TYPE:
        for (const char *p = j->text; *p && e == ESP_OK && !s_abort; p++) {
            kvm_chord_t c;
            kvm_ascii_chord(*p, &c);    /* 入隊前已校驗過 */
            e = tap(c);
        }
        break;
    case JOB_EXT:
        break;                          /* exec_task 單獨處理 */
    case JOB_MOUSE:
        switch (j->mop) {
        case 'm':
            e = kvm_hid_mouse(0, j->x, j->y, 0);
            break;
        case 'c':                       /* 先移過去停一下，再按；雙擊間隔 80ms */
            e = kvm_hid_mouse(0, j->x, j->y, 0);
            if (e == ESP_OK && !sleep_ms(40)) break;
            for (int i = 0; i < j->count && e == ESP_OK && !s_abort; i++) {
                if ((e = kvm_hid_mouse(j->btn, j->x, j->y, 0)) != ESP_OK || !sleep_ms(50)) break;
                if ((e = kvm_hid_mouse(0, j->x, j->y, 0)) != ESP_OK) break;
                sleep_ms(80);
            }
            break;
        case 's': {
            uint16_t x, y;
            kvm_hid_mouse_pos(&x, &y);
            int8_t step = j->count > 0 ? 1 : -1;
            for (int i = 0; i < abs(j->count) && e == ESP_OK && !s_abort; i++) {
                e = kvm_hid_mouse(0, x, y, step);
                sleep_ms(30);
            }
            break;
        }
        case 'd':                       /* 按住從 (x,y) 拖到 (x2,y2)，分 20 步 */
            e = kvm_hid_mouse(0, j->x, j->y, 0);
            if (e == ESP_OK && sleep_ms(40)) e = kvm_hid_mouse(j->btn, j->x, j->y, 0);
            for (int i = 1; i <= 20 && e == ESP_OK && sleep_ms(20); i++) {
                e = kvm_hid_mouse(j->btn, j->x + (j->x2 - j->x) * i / 20, j->y + (j->y2 - j->y) * i / 20, 0);
            }
            if (e == ESP_OK && sleep_ms(40)) e = kvm_hid_mouse(0, j->x2, j->y2, 0);
            break;
        }
        break;
    case JOB_REPEAT: {
        int64_t end = esp_timer_get_time() + (int64_t)j->total_ms * 1000;
        while (e == ESP_OK && !s_abort && esp_timer_get_time() < end) {
            int64_t t0 = esp_timer_get_time();
            e = tap(j->chords[0]);
            int spent = (int)((esp_timer_get_time() - t0) / 1000);
            if (spent < j->period_ms) sleep_ms(j->period_ms - spent);
        }
        break;
    }
    }
    return e;
}

static void exec_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *j;
        xQueueReceive(s_jobq, &j, portMAX_DELAY);
        if (j->kind == JOB_EXT) {       /* 不一定用 USB；回執由任務自己發 */
            j->fn(j->seq, j->arg);
            free(j);
            s_abort = false;
            s_busy = false;
            continue;
        }
        esp_err_t e = run_job(j);
        kvm_hid_release_all();
        if (s_abort) finish(j->seq, "err aborted:%s", s_abort_reason);
        else if (e != ESP_OK) finish(j->seq, "err usb:%s", esp_err_to_name(e));
        else finish(j->seq, "ok");
        free(j->text);
        free(j);
        s_abort = false;
        s_busy = false;
    }
}

/* ---------------- 各命令 ---------------- */

static const char *usb_state_str(void)
{
    switch (kvm_hid_usb_state()) {
    case KVM_USB_MOUNTED:   return "mounted";
    case KVM_USB_SUSPENDED: return "suspended";
    default:                return "detached";
    }
}

/*
 * 復位原因。注意 S3 硬件的原始碼 0x01 同時代表「上電」「芯片級欠壓」「超級看門狗」，
 * 電壓驟降到芯片復位閾值時硬件自己就報成上電，固件分不出來——所以另有開機計數 boots：
 * 兩次查詢之間 boots 變了就是重啟過，不管 reset 報什麼。
 * 電壓緩降時 IDF 的欠壓檢測（CONFIG_ESP_BROWNOUT_DET，閾值檔位 7）會先觸發，能正確報 brownout。
 */
static const char *reset_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "unknown";
    }
}

static uint32_t s_boots;

/* 每次開機在 NVS 裡加一 */
static void count_boot(void)
{
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u32(h, "boots", &s_boots);
    s_boots++;
    nvs_set_u32(h, "boots", s_boots);
    nvs_commit(h);
    nvs_close(h);
}

/* 外部組件註冊的信息鉤子：各自的狀態接在 state 回執後面 */
static void ext_state(char *buf, size_t len)
{
    size_t n = 0;
    buf[0] = '\0';
    for (int i = 0; i < s_ninfo && n + 2 < len; i++) {
        if (!s_info[i].state) continue;
        buf[n++] = ' ';
        s_info[i].state(buf + n, len - n);
        n += strlen(buf + n);
    }
}

static void cmd_state(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd; (void)args;
    char ble[96], ext[160];
    kvm_ble_info(ble, sizeof(ble));
    ext_state(ext, sizeof(ext));
    finish(seq, "ok usb=%s mode=%s proto=%s leds=%02x busy=%d uptime=%lld heap=%u psram=%u "
                "reset=%s raw=0x%02x boots=%" PRIu32 " %s%s",
           usb_state_str(), kvm_hid_mode_name(), kvm_hid_boot_protocol() ? "boot" : "report", kvm_hid_leds(), s_busy,
           esp_timer_get_time() / 1000000,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           reset_str(), (unsigned)esp_rom_get_reset_reason(0), s_boots, ble, ext);
}

static void cmd_caps(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd; (void)args;
    char ext[160] = "";
    for (int i = 0; i < s_ninfo; i++) {
        if (!s_info[i].caps) continue;
        strlcat(ext, " ", sizeof(ext));
        strlcat(ext, s_info[i].caps, sizeof(ext));
    }
    finish(seq, "ok ver=%s kbd=1 mouse=abs msc=ro_flash net=ncm_wifi_bridge modes=hid,hid+msc,msc,net hb_timeout_ms=%d%s",
           KVM_FW_VERSION, KVM_HB_TIMEOUT_MS, ext);
}

/* mode                              查詢當前 USB 身分和啟動盤統計
 * mode <hid|hid+msc|msc|net> [save] 切換（目標機看到拔掉再插上）；save = 同時存為開機模式（net 不能存）
 * net 會連 WiFi 並開始轉發，離開時關 WiFi；轉發的狀態用 net 命令看 */
static void cmd_mode(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd;
    char msc[260];
    if (*args) {
        if (s_busy) { finish(seq, "err busy"); return; }
        char *opt = strchr(args, ' ');
        if (opt) { *opt++ = '\0'; while (*opt == ' ') opt++; }
        bool save = opt && strcmp(opt, "save") == 0;
        if (opt && *opt && !save) { finish(seq, "err bad_arg:%s", opt); return; }
        bool want_net = strcmp(args, "net") == 0;
        if (!want_net) kvm_net_stop();          /* 先停轉發再換描述符，免得往已經不存在的網卡接口塞包 */
        if (kvm_hid_set_mode(args, save) != ESP_OK) { finish(seq, "err bad_mode:%s", args); return; }
        kvm_msc_rescan();
        if (want_net) {
            esp_err_t e = kvm_net_start();
            if (e != ESP_OK) { finish(seq, "err net:%s%s", esp_err_to_name(e), e == ESP_ERR_NOT_SUPPORTED ? "（固件裡沒有 WiFi 憑據）" : ""); return; }
        }
    }
    kvm_msc_info(msc, sizeof(msc));
    char cnt[96];
    kvm_hid_usb_counters(cnt, sizeof(cnt));
    finish(seq, "ok mode=%s usb=%s %s %s", kvm_hid_mode_name(), usb_state_str(), cnt, msc);
}

static void cmd_log(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd; (void)args;
    static char buf[3072];
    size_t n = logbuf_tail(buf, sizeof(buf) - 1);
    buf[n] = '\0';
    /* 第一行多半是被截斷的半行，丟掉 */
    char *p = (n == sizeof(buf) - 1) ? strchr(buf, '\n') : buf - 1;
    p = p ? p + 1 : buf;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        if (*p) send_fmt("| %s", p);
        if (!e) break;
        p = e + 1;
    }
    finish(seq, "ok");
}

/* type 的轉義：\n 回車，\t Tab，\\ 反斜槓 */
static char *decode_text(const char *s, const char **bad)
{
    char *out = malloc(strlen(s) + 1), *o = out;
    if (!out) return NULL;
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' && s[1]) {
            s++;
            c = (*s == 'n') ? '\n' : (*s == 't') ? '\t' : *s;
        }
        kvm_chord_t ch;
        if (!kvm_ascii_chord(c, &ch)) { *bad = s; free(out); return NULL; }
        *o++ = c;
    }
    *o = '\0';
    return out;
}

static void start_job(job_t *j)
{
    s_abort = false;
    s_busy = true;
    if (xQueueSend(s_jobq, &j, 0) != pdTRUE) {      /* 不應發生：s_busy 已保證隊列空 */
        s_busy = false;
        finish(j->seq, "err busy");
        free(j->text);
        free(j);
    }
}

/* key / type / repeat / move / click / scroll / drag：都要 USB，都放進執行任務 */
static void cmd_action(uint32_t seq, const char *cmd, char *args)
{
    if (s_busy) { finish(seq, "err busy"); return; }
    job_t *j = calloc(1, sizeof(*j));
    if (!j) { finish(seq, "err no_mem"); return; }
    j->seq = seq;

    if (strcmp(cmd, "key") == 0) {
        j->kind = JOB_CHORDS;
        for (char *save = NULL, *t = strtok_r(args, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
            if (j->n == MAX_CHORDS) { finish(seq, "err too_many_keys"); free(j); return; }
            if (!kvm_parse_chord(t, &j->chords[j->n++])) { finish(seq, "err bad_key:%s", t); free(j); return; }
        }
        if (j->n == 0) { finish(seq, "err no_key"); free(j); return; }
    } else if (strcmp(cmd, "type") == 0) {
        j->kind = JOB_TYPE;
        const char *bad = NULL;
        j->text = decode_text(args, &bad);
        if (!j->text) {
            if (bad) finish(seq, "err bad_char:0x%02x", (unsigned char)*bad);
            else finish(seq, "err no_mem");
            free(j);
            return;
        }
    } else if (!strcmp(cmd, "move") || !strcmp(cmd, "click") || !strcmp(cmd, "scroll") || !strcmp(cmd, "drag")) {
        /* move x y | click x y [left|right|middle] [次數] | scroll n | drag x1 y1 x2 y2 [鍵]，座標 0–32767 */
        j->kind = JOB_MOUSE;
        j->mop = !strcmp(cmd, "drag") ? 'd' : cmd[0];
        j->btn = 1;
        j->count = 1;
        char b[8] = "left";
        int n = 0;
        bool ok;
        if (j->mop == 's') ok = sscanf(args, "%d", &j->count) == 1 && j->count != 0 && abs(j->count) <= 50;
        else if (j->mop == 'd') ok = (n = sscanf(args, "%d %d %d %d %7s", &j->x, &j->y, &j->x2, &j->y2, b)) >= 4;
        else ok = (n = sscanf(args, "%d %d %7s %d", &j->x, &j->y, b, &j->count)) >= 2;
        if (ok && j->mop != 's') {
            j->btn = !strcmp(b, "left") ? 1 : !strcmp(b, "right") ? 2 : !strcmp(b, "middle") ? 4 : 0;
            ok = j->btn && j->count >= 1 && j->count <= 3 &&
                 j->x >= 0 && j->x <= 32767 && j->y >= 0 && j->y <= 32767 &&
                 j->x2 >= 0 && j->x2 <= 32767 && j->y2 >= 0 && j->y2 <= 32767;
        }
        if (!ok) {
            finish(seq, "err usage: move x y | click x y [left|right|middle] [1-3] | scroll n | drag x1 y1 x2 y2 [btn]（0-32767）");
            free(j);
            return;
        }
    } else {    /* repeat <和弦> <總毫秒> <間隔毫秒> */
        j->kind = JOB_REPEAT;
        char key[48];
        if (sscanf(args, "%47s %d %d", key, &j->total_ms, &j->period_ms) != 3 ||
            !kvm_parse_chord(key, &j->chords[0]) ||
            j->total_ms <= 0 || j->total_ms > 120000 || j->period_ms < 50) {
            finish(seq, "err usage: repeat <key> <total_ms<=120000> <period_ms>=50>");
            free(j);
            return;
        }
    }
    if (kvm_hid_usb_state() == KVM_USB_DETACHED) {
        finish(seq, "err usb_detached");
        free(j->text);
        free(j);
        return;
    }
    start_job(j);
}

static void cmd_hello(char *args)
{
    if (s_token_fail >= 5) return;              /* 本次連接已錯太多次，不再理會 */
    char token[64] = "", session[sizeof(s_session)] = "";
    sscanf(args, "%63s %39s", token, session);
    /* 定長比較，不因第一個不同的字節提前返回 */
    const char *want = KVM_TOKEN;
    size_t n = strlen(want);
    unsigned diff = strlen(token) != n;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)token[i] ^ (unsigned char)want[i];
    if (diff || !session[0]) {
        s_token_fail++;
        ESP_LOGW(TAG, "hello 認證失敗（第 %d 次）", s_token_fail);
        send_raw("0 err auth\n");
        return;
    }
    s_app_authed = true;
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    bool resumed = strcmp(session, s_session) == 0;
    if (!resumed) {             /* 新會話：清空去重狀態 */
        strlcpy(s_session, session, sizeof(s_session));
        s_last_seq = 0;
        s_last_done = true;
        s_last_reply[0] = '\0';
    }
    uint32_t last = s_last_seq;
    xSemaphoreGive(s_reply_lock);
    send_fmt("0 ok kvm %s session=%s last=%" PRIu32 " busy=%d", KVM_FW_VERSION,
             resumed ? "resumed" : "new", last, s_busy);
}

static void cmd_release(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd; (void)args;
    kvm_hid_release_all();
    finish(seq, "ok");
}

static void cmd_reboot(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd; (void)args;
    if (s_busy) { finish(seq, "err busy"); return; }
    finish(seq, "ok rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));     /* 讓回執先經 BLE 發出去 */
    esp_restart();
}

static void cmd_dfu(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd; (void)args;
    if (s_busy) { finish(seq, "err busy"); return; }
    finish(seq, "ok rebooting_to_download");
    kvm_hid_reboot_to_download();
}

static void handle_line(char *line)
{
    char *end;
    unsigned long seq = strtoul(line, &end, 10);
    if (end == line || (*end != ' ' && *end != '\0')) { send_raw("* err bad_line\n"); return; }
    while (*end == ' ') end++;
    char *cmd = end, *args = strchr(cmd, ' ');
    /* 命令名後只去掉一個分隔空格給 type：文本開頭的空格是內容（Python、YAML 的縮進） */
    if (args) { *args++ = '\0'; if (strcmp(cmd, "type") != 0) while (*args == ' ') args++; }
    else args = cmd + strlen(cmd);

    if (seq == 0) {             /* 不參與去重 */
        if (strcmp(cmd, "hb") == 0) return;
        if (strcmp(cmd, "hello") == 0) { cmd_hello(args); return; }
        if (!s_app_authed) { send_raw("0 err auth\n"); return; }
        if (strcmp(cmd, "abort") == 0) { request_abort("host"); return; }
        send_raw("0 err unknown\n");
        return;
    }
    if (!s_app_authed) { send_fmt("%lu err auth", seq); return; }

    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    if (seq == s_last_seq) {    /* 重發：已完成就重發回執，還在執行就忽略（完成時自然會回） */
        char again[sizeof(s_last_reply)] = "";
        if (s_last_done) strlcpy(again, s_last_reply, sizeof(again));
        xSemaphoreGive(s_reply_lock);
        if (again[0]) send_raw(again);
        return;
    }
    if (seq < s_last_seq) {
        uint32_t last = s_last_seq;
        xSemaphoreGive(s_reply_lock);
        send_fmt("%lu err stale last=%" PRIu32, seq, last);
        return;
    }
    s_last_seq = seq;
    s_last_done = false;
    s_last_reply[0] = '\0';
    xSemaphoreGive(s_reply_lock);

    for (int i = 0; i < s_ncmds; i++) {
        if (strcmp(cmd, s_cmds[i].name) == 0) { s_cmds[i].fn((uint32_t)seq, cmd, args); return; }
    }
    finish(seq, "err unknown_cmd:%s", cmd);
}

/* ---------------- 主循環 ---------------- */

static void cmd_task(void *arg)
{
    (void)arg;
    for (;;) {
        item_t it;
        if (xQueueReceive(s_q, &it, pdMS_TO_TICKS(200)) == pdTRUE) {
            switch (it.kind) {
            case ITEM_LINE:  if (it.text) handle_line(it.text); break;
            case ITEM_EVENT: if (it.text) send_raw(it.text); break;
            case ITEM_LINK_UP:
                s_app_authed = false;
                s_token_fail = 0;
                send_fmt("* ready kvm %s", KVM_FW_VERSION);
                break;
            case ITEM_LINK_DOWN:
                s_app_authed = false;
                break;
            }
            free(it.text);
        }
        if (s_busy && esp_timer_get_time() - s_last_rx_us > (int64_t)KVM_HB_TIMEOUT_MS * 1000) {
            request_abort("hb_timeout");
        }
    }
}

/* ---------------- 給外部組件的接口（說明見 kvm_cmd.h） ---------------- */

esp_err_t kvm_cmd_register(const char *name, kvm_cmd_fn_t fn)
{
    if (!name || !fn) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < s_ncmds; i++) if (strcmp(name, s_cmds[i].name) == 0) return ESP_ERR_INVALID_STATE;
    if (s_ncmds == MAX_CMDS) return ESP_ERR_NO_MEM;
    s_cmds[s_ncmds].name = name;
    s_cmds[s_ncmds].fn = fn;
    s_ncmds++;
    return ESP_OK;
}

esp_err_t kvm_cmd_register_info(kvm_info_fn_t state, const char *caps)
{
    if (s_ninfo == MAX_INFO) return ESP_ERR_NO_MEM;
    s_info[s_ninfo].state = state;
    s_info[s_ninfo].caps = caps;
    s_ninfo++;
    return ESP_OK;
}

void kvm_cmd_reply(uint32_t seq, const char *fmt, ...)
{
    char body[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    finish(seq, "%s", body);
}

bool kvm_cmd_send(const char *data, size_t len) { return kvm_link_send(data, len); }
bool kvm_cmd_busy(void) { return s_busy; }
bool kvm_cmd_aborted(void) { return s_abort; }
const char *kvm_cmd_abort_reason(void) { return s_abort_reason; }
void kvm_cmd_abort(const char *reason) { request_abort(reason); }

esp_err_t kvm_cmd_start_job(uint32_t seq, kvm_job_fn_t fn, void *arg)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    job_t *j = calloc(1, sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    j->kind = JOB_EXT;
    j->seq = seq;
    j->fn = fn;
    j->arg = arg;
    s_abort = false;
    s_busy = true;
    if (xQueueSend(s_jobq, &j, 0) != pdTRUE) {      /* 不應發生：s_busy 已保證隊列空 */
        s_busy = false;
        free(j);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void kvm_cmd_init(void)
{
    count_boot();                       /* main 已初始化 NVS */
    static const struct { const char *name; kvm_cmd_fn_t fn; } builtin[] = {
        { "state", cmd_state }, { "caps", cmd_caps }, { "log", cmd_log }, { "mode", cmd_mode },
        { "release", cmd_release }, { "reboot", cmd_reboot }, { "dfu", cmd_dfu },
        { "key", cmd_action }, { "type", cmd_action }, { "repeat", cmd_action },
        { "move", cmd_action }, { "click", cmd_action }, { "scroll", cmd_action }, { "drag", cmd_action },
    };
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) kvm_cmd_register(builtin[i].name, builtin[i].fn);
    s_reply_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(16, sizeof(item_t));
    s_jobq = xQueueCreate(1, sizeof(job_t *));
    xTaskCreate(cmd_task, "kvm_cmd", 6144, NULL, 6, NULL);
    xTaskCreate(exec_task, "kvm_exec", 4096, NULL, 7, NULL);
}
