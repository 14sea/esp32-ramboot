/*
 * USB HID：Boot Protocol 鍵盤（BIOS 只認這種）+ 絕對座標滑鼠，跑在原生 USB 口上接目標電腦。
 * 兩個獨立接口：實例 0 鍵盤（Boot），實例 1 滑鼠（非 Boot，BIOS 不需要它，座標 0–32767 映射到整個桌面）。
 * 接目標的線已切斷 VBUS（S3-01 兩個 USB 口的 VBUS 直連、會倒灌），
 * IDF 沒配 VBUS 偵測腳時默認 VBUS 一直有效，所以照常枚舉。
 */
#include "kvm_hid.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_cmd.h"
#include "kvm_msc.h"
#include "kvm_net.h"
#include "nvs.h"
#include "soc/rtc_cntl_reg.h"
#include "device/dcd.h"
#include "tusb.h"

static const char *TAG = "kvm_hid";

static usb_phy_handle_t s_phy;
static SemaphoreHandle_t s_send_lock;
static volatile int64_t s_mount_us;
static volatile bool s_need_prime;
static volatile uint8_t s_leds;
static uint16_t s_mx = 16384, s_my = 16384;   /* 滑鼠最後位置，放開按鈕時原地放 */
static volatile uint8_t s_mbtn;

/* ---------------- 描述符 ---------------- */

/*
 * USB 身分可切換（kvm_hid_set_mode），每種模式一套配置描述符：
 *   hid      鍵盤 + 滑鼠
 *   hid+msc  鍵盤 + 滑鼠 + 只讀啟動盤（默認）：要按 F12 選啟動項、再從這塊盤啟動時用
 *   msc      只有啟動盤：給不認複合設備的 BIOS
 *   net      鍵盤 + 啟動盤 + USB 網卡（CDC-NCM，見 kvm_net.c）：目標機沒有可用網卡時，等它的小系統起來之後換上，
 *            經這塊板子的 WiFi 上網。沒有滑鼠——端點不夠；啟動盤留著，是為了反饋通道（kvm_target.c）還能用
 * PID 按 esp_tinyusb 的編號規則 0x4000 | MSC 位(2) | HID 位(4)，網卡自定為 0x10；每種模式不同，主機不會套用舊的緩存。
 * IN 端點硬上限 4 個（不含 EP0）：鍵盤 1 + 滑鼠 1 + 啟動盤 1 = 3；鍵盤 1 + 啟動盤 1 + 網卡 2 = 4，剛好用滿。
 */
#define USB_VID 0x303A          /* Espressif VID 僅限自用/測試 */

enum { ITF_NUM_KBD = 0, ITF_NUM_MOUSE, ITF_NUM_HID_TOTAL, ITF_NUM_MSC = ITF_NUM_HID_TOTAL, ITF_NUM_HIDMSC_TOTAL };
enum { ITF_NET_KBD = 0, ITF_NET_MSC, ITF_NET_NCM, ITF_NET_NCM_DATA, ITF_NET_TOTAL };
enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL, STRID_KBD, STRID_MOUSE, STRID_MSC, STRID_NCM, STRID_NCM_MAC };
enum { HID_KBD = 0, HID_MOUSE = 1 };   /* TinyUSB 的 HID 實例號，按接口出現順序 */

#define EPNUM_KBD_IN   0x81
#define EPNUM_MOUSE_IN 0x82
#define EPNUM_MSC_OUT  0x03
#define EPNUM_MSC_IN   0x83
/* net 模式：啟動盤改用滑鼠讓出來的那對端點號，IN 端點號必須在 0x81–0x84 之內 */
#define EPNUM_NET_MSC_OUT 0x02
#define EPNUM_NET_MSC_IN  0x82
#define EPNUM_NCM_NOTIF   0x83
#define EPNUM_NCM_OUT     0x04
#define EPNUM_NCM_IN      0x84

static const uint8_t s_report_desc[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const uint8_t s_mouse_desc[]  = { TUD_HID_REPORT_DESC_ABSMOUSE() };

static const struct { const char *name; uint16_t pid; const char *product; } s_modes[KVM_USBMODE_COUNT] = {
    [KVM_USBMODE_HID]     = { "hid",     0x4004, "S3 KVM Keyboard" },
    [KVM_USBMODE_HID_MSC] = { "hid+msc", 0x4006, "S3 KVM Keyboard" },   /* 產品名不變：目標機上按名字找鍵盤的腳本照常用 */
    [KVM_USBMODE_MSC]     = { "msc",     0x4002, "S3 KVM Boot Disk" },
    [KVM_USBMODE_NET]     = { "net",     0x4016, "S3 KVM Keyboard" },
};
/* 沒存過開機模式時帶啟動盤：新板子不經藍牙也能直接拿來啟動 */
static volatile kvm_usb_mode_t s_mode = KVM_USBMODE_HID_MSC;
#define MODE_HAS_HID() (s_mode != KVM_USBMODE_MSC)
#define MODE_HAS_MOUSE() (s_mode == KVM_USBMODE_HID || s_mode == KVM_USBMODE_HID_MSC)

static tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = 0x4006,          /* 每次取描述符時按當前模式填 */
    .bcdDevice          = 0x0300,          /* 0x0200：加了滑鼠接口；0x0300：可切換模式 */
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

/* 自供電（充電頭供電、不取目標的 VBUS）+ 允許遠程喚醒；申報 100mA */
#define CFG_ATTR (TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP)
/* 接口協議 = 鍵盤 → 宏裡自動把 subclass 設成 Boot */
#define DESC_KBD   TUD_HID_DESCRIPTOR(ITF_NUM_KBD, STRID_KBD, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_report_desc), \
                                      EPNUM_KBD_IN, CFG_TUD_HID_EP_BUFSIZE, 5)
#define DESC_MOUSE TUD_HID_DESCRIPTOR(ITF_NUM_MOUSE, STRID_MOUSE, HID_ITF_PROTOCOL_NONE, sizeof(s_mouse_desc), \
                                      EPNUM_MOUSE_IN, CFG_TUD_HID_EP_BUFSIZE, 5)

static const uint8_t s_cfg_hid[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_HID_TOTAL, 0, TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN, CFG_ATTR, 100),
    DESC_KBD, DESC_MOUSE,
};
static const uint8_t s_cfg_hid_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_HIDMSC_TOTAL, 0, TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN, CFG_ATTR, 100),
    DESC_KBD, DESC_MOUSE,
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};
static const uint8_t s_cfg_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_MSC_DESCRIPTOR(0, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static const uint8_t s_cfg_net[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NET_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN + TUD_CDC_NCM_DESC_LEN, CFG_ATTR, 100),
    DESC_KBD,
    TUD_MSC_DESCRIPTOR(ITF_NET_MSC, STRID_MSC, EPNUM_NET_MSC_OUT, EPNUM_NET_MSC_IN, 64),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NET_NCM, STRID_NCM, STRID_NCM_MAC, EPNUM_NCM_NOTIF, 64, EPNUM_NCM_OUT, EPNUM_NCM_IN, 64, 1514),
};

static char s_serial[13];

uint8_t const *tud_descriptor_device_cb(void)
{
    s_desc_device.idProduct = s_modes[s_mode].pid;
    /* 網卡描述符帶接口關聯（IAD），設備類要報成 Misc/IAD；其餘模式保持 0（由接口決定），老 BIOS 最認這個 */
    bool iad = s_mode == KVM_USBMODE_NET;
    s_desc_device.bDeviceClass    = iad ? TUSB_CLASS_MISC : 0;
    s_desc_device.bDeviceSubClass = iad ? MISC_SUBCLASS_COMMON : 0;
    s_desc_device.bDeviceProtocol = iad ? MISC_PROTOCOL_IAD : 0;
    return (uint8_t const *)&s_desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    switch (s_mode) {
    case KVM_USBMODE_HID_MSC: return s_cfg_hid_msc;
    case KVM_USBMODE_MSC:     return s_cfg_msc;
    case KVM_USBMODE_NET:     return s_cfg_net;
    default:                  return s_cfg_hid;
    }
}
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == HID_MOUSE ? s_mouse_desc : s_report_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    const char *str;
    switch (index) {
    case STRID_LANGID:
        buf[0] = (TUSB_DESC_STRING << 8) | 4;
        buf[1] = 0x0409;
        return buf;
    case STRID_MANUFACTURER: str = "DIY"; break;
    case STRID_PRODUCT:      str = s_modes[s_mode].product; break;
    case STRID_SERIAL:       str = s_serial; break;
    case STRID_KBD:          str = "S3 KVM Keyboard"; break;
    case STRID_MOUSE:        str = "S3 KVM Mouse"; break;
    case STRID_MSC:          str = "S3 KVM Boot Disk"; break;
    case STRID_NCM:          str = "S3 KVM Network"; break;
    case STRID_NCM_MAC:      str = s_serial; break;     /* 主機側網卡 MAC = STA MAC，正好就是序列號那 12 位十六進制 */
    default:                 return NULL;
    }
    size_t n = strlen(str);
    if (n > 31) n = 31;
    for (size_t i = 0; i < n; i++) buf[1 + i] = str[i];
    buf[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return buf;
}

/* ---------------- 回調 ---------------- */

/* 總線事件計數：排查「主機換了一個驅動/內核之後設備不應答」時，要知道板子到底有沒有收到復位 */
static volatile uint32_t s_n_reset, s_n_mount, s_n_setup, s_n_suspend, s_n_resume;

/*
 * 「下次目標機重啟時換模式」（mode next，或目標機發 SCSI 廠商命令 NEXT）：
 * 板子自供電，目標機重啟時板子不斷電，看到的只是一次總線復位，之後主機重新枚舉。
 * 在復位那一刻（中斷裡，事件入隊之後、主機來取描述符之前）換掉模式，固件枚舉時看到的就已經是新身分。
 * 用途：裝好硬盤之後重啟，優先從 U 盤啟動的固件不會又進 Stage-1；開機模式（NVS）不變。
 * 只生效一次。back_s > 0 時，生效後再過這麼多秒換回原來的模式（硬盤系統起來後還能用反饋通道和網卡）。
 * 收尾（停網卡、發事件、到時換回）在 s_next_task 裡做，tud_task() 不返回，不能放在 USB 任務裡。
 */
static volatile int8_t s_next = -1;             /* 待生效的模式，-1 = 沒有 */
static volatile uint16_t s_next_back_s;
static volatile int8_t s_applied_from = -1;     /* 生效時原來的模式 */
static volatile uint32_t s_mode_gen;            /* 每次立即切換加一：換回之前有人手動切過，就不換回 */
static TaskHandle_t s_next_task;
/* 給 s_next_task 的通知（按位）：生效了 / 取消正在倒數的換回 */
#define NEXT_APPLIED 0x1u
#define NEXT_CANCEL  0x2u
static volatile int8_t s_restore_to = -1;       /* 正在倒數、到時要換回的模式，-1 = 沒有 */
static volatile int64_t s_restore_at_us;

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;
    switch (eventid) {
    case DCD_EVENT_BUS_RESET:
        s_n_reset++;
        if (s_next >= 0) {
            s_applied_from = s_mode;
            s_mode = (kvm_usb_mode_t)s_next;
            s_next = -1;
            s_leds = 0;
            s_mbtn = 0;
            if (s_next_task) {
                if (in_isr) xTaskNotifyFromISR(s_next_task, NEXT_APPLIED, eSetBits, NULL);
                else xTaskNotify(s_next_task, NEXT_APPLIED, eSetBits);
            }
        }
        break;
    case DCD_EVENT_SETUP_RECEIVED: s_n_setup++; break;
    case DCD_EVENT_SUSPEND:        s_n_suspend++; break;
    case DCD_EVENT_RESUME:         s_n_resume++; break;
    default: break;
    }
}

void kvm_hid_usb_counters(char *buf, size_t len)
{
    snprintf(buf, len, "usb_rst=%" PRIu32 " usb_mnt=%" PRIu32 " usb_setup=%" PRIu32 " usb_susp=%" PRIu32 " usb_resume=%" PRIu32,
             s_n_reset, s_n_mount, s_n_setup, s_n_suspend, s_n_resume);
}

void tud_mount_cb(void)
{
    s_mount_us = esp_timer_get_time();
    s_n_mount++;
    s_need_prime = true;
    kvm_msc_bus_reset();            /* 目標機重啟後重新枚舉：被彈出的啟動盤放回去 */
    kvm_cmd_event("usb mounted mode=%s", s_modes[s_mode].name);
}

void tud_umount_cb(void)   { kvm_cmd_event("usb detached"); }
void tud_suspend_cb(bool remote_wakeup_en) { kvm_cmd_event(remote_wakeup_en ? "usb suspended wakeup=1" : "usb suspended wakeup=0"); }
void tud_resume_cb(void)   { kvm_cmd_event("usb resumed"); }

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    if (instance != HID_KBD) return;
    kvm_cmd_event(protocol == HID_PROTOCOL_BOOT ? "usb protocol=boot" : "usb protocol=report");
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)type;
    /* GET_REPORT 很少用；回當前「沒有鍵按下」 */
    uint16_t n = reqlen < 8 ? reqlen : 8;
    memset(buffer, 0, n);
    return n;
}

/*
 * 鍵盤燈（主機 → 設備的 Output 報告）。
 * 另外藏一條不經 BLE 的「進下載模式」通道：Compose 和 Kana 兩個燈同時亮的狀態
 * 3 秒內出現 3 次。正常系統幾乎不會碰這兩個燈，Linux 上可以經
 * /sys/class/leds/inputN::compose|kana 精確控制——新固件的 BLE 起不來時還能從目標電腦重燒。
 */
#define MAGIC_LEDS (KEYBOARD_LED_COMPOSE | KEYBOARD_LED_KANA)

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    if (instance != HID_KBD || type != HID_REPORT_TYPE_OUTPUT || bufsize < 1) return;
    static int magic_n;
    static int64_t magic_t0;
    uint8_t prev = s_leds, now = buffer[0];
    s_leds = now;
    if (now != prev) kvm_cmd_event("leds %02x", now);
    if ((now & MAGIC_LEDS) == MAGIC_LEDS && (prev & MAGIC_LEDS) != MAGIC_LEDS) {
        int64_t t = esp_timer_get_time();
        if (magic_n == 0 || t - magic_t0 > 3000000) { magic_n = 0; magic_t0 = t; }
        if (++magic_n >= 3) {
            magic_n = 0;
            ESP_LOGW(TAG, "收到鍵盤燈暗號，進下載模式");
            kvm_hid_reboot_to_download();
        }
    }
}

/* ---------------- 發報告 ---------------- */

static bool wait_ready_n(uint8_t inst, int ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (!tud_hid_n_ready(inst)) {
        if (esp_timer_get_time() > end) return false;
        vTaskDelay(1);
    }
    return true;
}

static bool wait_ready(int ms) { return wait_ready_n(HID_KBD, ms); }

/* 掛載、喚醒、首報告被丟的處理，鍵盤和滑鼠共用 */
static esp_err_t prepare_locked(void)
{
    if (!MODE_HAS_HID()) return ESP_ERR_NOT_SUPPORTED;      /* 純啟動盤模式沒有鍵盤滑鼠 */
    if (!tud_mounted()) return ESP_ERR_INVALID_STATE;
    if (tud_suspended()) {
        tud_remote_wakeup();
        for (int i = 0; i < 100 && tud_suspended(); i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (tud_suspended()) return ESP_ERR_TIMEOUT;
    }
    /* 主機剛 configure 時第一份報告常被丟（GoodUSB 的經驗）：等滿 700ms，先送一份空報告 */
    int64_t since = esp_timer_get_time() - s_mount_us;
    if (since < 700000) vTaskDelay(pdMS_TO_TICKS((700000 - since) / 1000 + 1));
    if (s_need_prime) {
        s_need_prime = false;
        if (wait_ready(100)) tud_hid_keyboard_report(0, 0, NULL);
    }
    return ESP_OK;
}

static esp_err_t send_locked(uint8_t mod, uint8_t key)
{
    esp_err_t err = prepare_locked();
    if (err != ESP_OK) return err;
    if (!wait_ready(100)) return ESP_ERR_TIMEOUT;
    uint8_t keys[6] = { key, 0, 0, 0, 0, 0 };
    return tud_hid_keyboard_report(0, mod, keys) ? ESP_OK : ESP_FAIL;
}

esp_err_t kvm_hid_send(uint8_t mod, uint8_t key)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_err_t err = send_locked(mod, key);
    xSemaphoreGive(s_send_lock);
    return err;
}

esp_err_t kvm_hid_mouse(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel)
{
    if (x > 32767) x = 32767;
    if (y > 32767) y = 32767;
    if (!MODE_HAS_MOUSE()) return ESP_ERR_NOT_SUPPORTED;        /* 純啟動盤、網卡模式沒有滑鼠 */
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_err_t err = prepare_locked();
    if (err == ESP_OK && !wait_ready_n(HID_MOUSE, 100)) err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        err = tud_hid_n_abs_mouse_report(HID_MOUSE, 0, buttons, (int16_t)x, (int16_t)y, wheel, 0) ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) { s_mx = x; s_my = y; s_mbtn = buttons; }
    }
    xSemaphoreGive(s_send_lock);
    return err;
}

void kvm_hid_mouse_pos(uint16_t *x, uint16_t *y) { *x = s_mx; *y = s_my; }

void kvm_hid_release_all(void)
{
    if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (MODE_HAS_HID() && tud_mounted() && !tud_suspended()) {
        if (wait_ready(50)) tud_hid_keyboard_report(0, 0, NULL);
        if (s_mbtn && MODE_HAS_MOUSE() && wait_ready_n(HID_MOUSE, 50)) {      /* 滑鼠按鈕原地放開 */
            tud_hid_n_abs_mouse_report(HID_MOUSE, 0, 0, (int16_t)s_mx, (int16_t)s_my, 0, 0);
            s_mbtn = 0;
        }
    }
    xSemaphoreGive(s_send_lock);
}

kvm_usb_state_t kvm_hid_usb_state(void)
{
    if (!tud_mounted()) return KVM_USB_DETACHED;
    return tud_suspended() ? KVM_USB_SUSPENDED : KVM_USB_MOUNTED;
}

uint8_t kvm_hid_leds(void) { return s_leds; }
bool kvm_hid_boot_protocol(void) { return MODE_HAS_HID() && tud_mounted() && tud_hid_get_protocol() == HID_PROTOCOL_BOOT; }

/* ---------------- 模式切換 ---------------- */

kvm_usb_mode_t kvm_hid_mode(void) { return s_mode; }
const char *kvm_hid_mode_name(void) { return s_modes[s_mode].name; }

static void save_mode(kvm_usb_mode_t m)
{
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "usbmode", (uint8_t)m);
    nvs_commit(h);
    nvs_close(h);
}

/*
 * 換 USB 身分：撤 D+ 上拉 → 等主機確認「拔出」→ 換描述符 → 重新接上，主機當成拔掉舊設備、插上新設備。
 * 不會切斷 BLE 控制通道，所以不需要「逾時未確認自動回滾」；每種模式也都留有從目標機進下載模式的路
 * （有鍵盤時用鍵盤燈暗號，純啟動盤時用 SCSI 廠商命令）。
 */
esp_err_t kvm_hid_set_mode(const char *name, bool save)
{
    int m = -1;
    for (int i = 0; i < KVM_USBMODE_COUNT; i++) if (strcmp(name, s_modes[i].name) == 0) m = i;
    if (m < 0) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    s_next = -1;                /* 立即切換取消待生效的 next（不然下面斷開重連引起的復位就會觸發它） */
    s_mode_gen++;
    if (s_restore_to >= 0) {    /* 也取消正在倒數的換回（gen 已經加過一） */
        s_restore_to = -1;
        if (s_next_task) xTaskNotify(s_next_task, NEXT_CANCEL, eSetBits);
    }
    if ((kvm_usb_mode_t)m != s_mode) {
        if (MODE_HAS_HID() && tud_mounted() && !tud_suspended() && wait_ready(50)) tud_hid_keyboard_report(0, 0, NULL);
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(600));
        s_mode = (kvm_usb_mode_t)m;
        s_leds = 0;
        s_mbtn = 0;
        tud_connect();
    }
    xSemaphoreGive(s_send_lock);
    if (save && m != KVM_USBMODE_NET) save_mode((kvm_usb_mode_t)m);
    return ESP_OK;
}

esp_err_t kvm_hid_set_next(const char *name, int back_s)
{
    if (!name || !*name || !strcmp(name, "off")) {      /* 取消待生效的 next，和已經生效、正在倒數的換回 */
        s_next = -1;
        if (s_restore_to >= 0) {
            /* 同步清掉，回執馬上看得到；gen 加一：倒數恰好在這時到期，任務也會跳過換回 */
            s_restore_to = -1;
            s_mode_gen++;
            if (s_next_task) xTaskNotify(s_next_task, NEXT_CANCEL, eSetBits);
        }
        return ESP_OK;
    }
    int m = -1;
    for (int i = 0; i < KVM_USBMODE_COUNT; i++) if (strcmp(name, s_modes[i].name) == 0) m = i;
    /* net 不能作 next：重啟後的固件階段用不上網卡，而且要先連 WiFi */
    if (m < 0 || m == KVM_USBMODE_NET) return ESP_ERR_NOT_FOUND;
    if (back_s < 0 || back_s > 3600) return ESP_ERR_INVALID_ARG;
    s_next_back_s = (uint16_t)back_s;
    s_next = (int8_t)m;
    return ESP_OK;
}

void kvm_hid_next_info(char *buf, size_t len)
{
    int8_t n = s_next, r = s_restore_to;
    int k = n < 0 ? snprintf(buf, len, "next=-")
                  : snprintf(buf, len, "next=%s back_s=%u", s_modes[n].name, (unsigned)s_next_back_s);
    if (r >= 0 && k > 0 && (size_t)k < len) {
        int64_t left = (s_restore_at_us - esp_timer_get_time()) / 1000000;
        snprintf(buf + k, len - k, " restore=%s in=%llds", s_modes[r].name, left > 0 ? left : 0);
    }
}

/* 換到 name：和 mode 命令同樣的順序（離開 net 先停轉發，進 net 後才連 WiFi） */
static void switch_mode(const char *name)
{
    bool want_net = strcmp(name, "net") == 0;
    if (!want_net) kvm_net_stop();
    kvm_hid_set_mode(name, false);
    kvm_msc_rescan();
    if (want_net && kvm_net_start() != ESP_OK) kvm_cmd_event("usb restore err net");
}

static void next_task(void *arg)
{
    (void)arg;
    uint32_t bits = 0;
    for (;;) {
        if (!(bits & NEXT_APPLIED)) {
            xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
            if (!(bits & NEXT_APPLIED)) continue;           /* 只有取消：沒有在倒數，不用管 */
        }
        bits = 0;
        int8_t from = s_applied_from;
        kvm_usb_mode_t to = s_mode;
        uint16_t back = s_next_back_s;
        uint32_t gen = s_mode_gen;
        if (from < 0) continue;
        if (from == KVM_USBMODE_NET && to != KVM_USBMODE_NET) kvm_net_stop();     /* 網卡接口已經不在了 */
        kvm_msc_rescan();
        kvm_cmd_event("usb next applied mode=%s from=%s back_s=%u", s_modes[to].name, s_modes[from].name, (unsigned)back);
        if (!back) continue;
        /* 等 back 秒。期間又一次 next 生效：按新的那次重新開始；mode next off 或立即切換：取消換回 */
        s_restore_at_us = esp_timer_get_time() + (int64_t)back * 1000000;
        s_restore_to = from;
        BaseType_t woke = xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS((uint32_t)back * 1000));
        s_restore_to = -1;
        if (woke && (bits & NEXT_APPLIED)) continue;
        if (woke && (bits & NEXT_CANCEL)) { bits = 0; kvm_cmd_event("usb restore cancelled"); continue; }
        bits = 0;
        if (gen != s_mode_gen || s_mode != to) { kvm_cmd_event("usb restore skipped (mode changed)"); continue; }
        switch_mode(s_modes[from].name);
        kvm_cmd_event("usb restored mode=%s", s_modes[from].name);
    }
}

/* ---------------- 進下載模式 ---------------- */

static void reboot_to_download(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "切回 USB-Serial-JTAG 並重啟進下載模式");
    /* 先撤 D+ 上拉停 0.5 秒，讓主機明確看到「拔出」，否則它不會去枚舉新的下載口 */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    usb_del_phy(s_phy);
    usb_phy_config_t usj = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target     = USB_PHY_TARGET_INT,
    };
    usb_phy_handle_t h;
    usb_new_phy(&usj, &h);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void kvm_hid_reboot_to_download(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = { .callback = reboot_to_download, .name = "dl" };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 300 * 1000);    /* 先讓當前的 USB 控制傳輸 / BLE 回執發完 */
}

/* ---------------- 啟動 ---------------- */

static void usb_task(void *arg)
{
    (void)arg;
    if (!tusb_init()) {
        ESP_LOGE(TAG, "TinyUSB 初始化失敗");
        vTaskDelete(NULL);
    }
    /*
     * 開機時明確「拔出」0.5 秒再接上。復位後 ROM 階段口上是 USB-Serial-JTAG，固件接管後換成 OTG，
     * 兩者都拉著 D+，中間的空檔很短：經集線器接主機時集線器察覺不到換了設備，主機一直以為還是下載口，
     * 新設備永遠等不到總線復位（X220 + Genesys 集線器上實測 usb_rst=0、usb=detached，直連和 usbip 時碰巧沒事）。
     */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_connect();
    for (;;) tud_task();
}

esp_err_t kvm_hid_start(void)
{
    s_send_lock = xSemaphoreCreateMutex();
    if (xTaskCreate(next_task, "usbnext", 3072, NULL, 5, &s_next_task) != pdPASS) return ESP_ERR_NO_MEM;
    kvm_msc_init();
    kvm_net_init();
    nvs_handle_t h;                 /* 開機模式：上次 "mode … save" 存下的，沒存過就是 hid+msc */
    if (nvs_open("kvm", NVS_READONLY, &h) == ESP_OK) {
        uint8_t m;
        /* 網卡模式不作開機模式：它要連 WiFi，而且目標機的固件未必肯從帶網卡的複合設備啟動 */
        if (nvs_get_u8(h, "usbmode", &m) == ESP_OK && m < KVM_USBMODE_COUNT && m != KVM_USBMODE_NET) s_mode = (kvm_usb_mode_t)m;
        nvs_close(h);
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
    };
    esp_err_t err = usb_new_phy(&phy_conf, &s_phy);
    if (err != ESP_OK) return err;
    return xTaskCreatePinnedToCore(usb_task, "usb", 4096, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
