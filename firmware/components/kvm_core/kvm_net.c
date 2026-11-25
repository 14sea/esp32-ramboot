/*
 * USB 網卡（USB 模式 net 時啟用）：目標機把這塊板子當成免驅的 CDC-NCM 網卡，以太網幀在 USB 和 WiFi 之間原樣二層轉發。
 * 給「目標機沒有可用網卡 / 沒有網線」的情況用：目標機只要有 CPU、內存和一個 USB 口。
 *
 * WiFi AP 只收源 MAC 等於已關聯 STA 的幀，所以讓目標機那一側的網卡 MAC = S3 的 STA MAC：
 * 對路由器來說目標機「就是」這塊 S3，自己走 DHCP 拿到局域網地址，agent 可以直接 SSH 過去，
 * 不需要 NAT 和端口映射（做法同 IDF 的 examples/network/sta2eth）。
 * 代價：這個模式下 S3 自己的 IP 協議棧收不到包；控制通道仍是 BLE，不受影響（BLE 連接間隔會放慢，給 WiFi 讓出射頻）。
 * 全速 USB 口加上 BLE 共存，實測轉發速率約 400–450 KB/s。
 *
 * S3 不看包的內容，唯一例外是順手記下路由器發給目標機的 DHCP ACK 裡的地址報給 agent——
 * 否則 agent 不知道目標機拿到了哪個 IP。
 */
#include "kvm_net.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include "board_wifi.h"
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID ""
#endif
#include "device/usbd_pvt.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_private/wifi.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kvm_ble.h"
#include "kvm_cmd.h"
#include "nvs.h"
#include "tusb.h"

static const char *TAG = "kvm_net";

/* NCM 描述符裡報給主機的「主機側網卡 MAC」，TinyUSB 要求這個全局符號 */
uint8_t tud_network_mac_address[6];

typedef struct { void *buf; void *eb; uint16_t len; } pkt_t;

static QueueHandle_t s_q;               /* WiFi → USB 待發隊列，元素持有 WiFi 的接收緩衝 */
static esp_timer_handle_t s_pump_timer;
static volatile bool s_want, s_bridging, s_pump_pending, s_handlers;
static volatile enum { NET_OFF, NET_CONNECTING, NET_UP, NET_FAILED } s_state;
static TaskHandle_t s_task;
static uint8_t s_ip[4];
static struct { uint32_t to_usb, to_wifi, drop_q, drop_tx; uint64_t to_usb_b, to_wifi_b; } s_st;

#define QLEN 24                         /* 不能太長：隊列裡每個包都佔著一個 WiFi 接收緩衝 */


/* ---------------- WiFi → USB ---------------- */

/* 路由器回給目標機的 DHCP ACK：以太網 14 + IP + UDP(67→68) + BOOTP，yiaddr 在 BOOTP 偏移 16 */
static void sniff_dhcp_ack(const uint8_t *f, uint16_t len)
{
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 17) return;
    unsigned ihl = (f[14] & 0x0f) * 4;
    const uint8_t *udp = f + 14 + ihl;
    if (len < 14 + ihl + 8 + 240 || udp[0] != 0 || udp[1] != 67 || udp[2] != 0 || udp[3] != 68) return;
    const uint8_t *bp = udp + 8, *end = f + len;
    if (bp[0] != 2 || memcmp(bp + 236, "\x63\x82\x53\x63", 4) != 0) return;
    for (const uint8_t *o = bp + 240; o + 2 <= end && *o != 255; ) {
        if (*o == 0) { o++; continue; }
        if (*o == 53 && o[1] == 1 && o + 3 <= end) {
            if (o[2] == 5 && memcmp(s_ip, bp + 16, 4) != 0) {
                memcpy(s_ip, bp + 16, 4);
                kvm_cmd_event("net ip=%u.%u.%u.%u", s_ip[0], s_ip[1], s_ip[2], s_ip[3]);
            }
            return;
        }
        o += 2 + o[1];
    }
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    memcpy(dst, ((pkt_t *)ref)->buf, arg);
    return arg;
}

/* 在 TinyUSB 任務裡跑（NCM 驅動的狀態沒有鎖，只能在它自己的任務裡碰） */
static void pump(void *arg)
{
    (void)arg;
    s_pump_pending = false;
    pkt_t p;
    while (xQueuePeek(s_q, &p, 0) == pdTRUE) {
        if (!s_bridging || !tud_ready()) {              /* 主機不在了：丟掉 */
            xQueueReceive(s_q, &p, 0);
            esp_wifi_internal_free_rx_buffer(p.eb);
            continue;
        }
        if (!tud_network_can_xmit(p.len)) return;       /* 發送緩衝滿，等定時器下一拍再來 */
        xQueueReceive(s_q, &p, 0);
        tud_network_xmit(&p, p.len);
        esp_wifi_internal_free_rx_buffer(p.eb);
        s_st.to_usb++;
        s_st.to_usb_b += p.len;
    }
}

static void kick_pump(void)
{
    if (s_pump_pending) return;
    s_pump_pending = true;
    usbd_defer_func(pump, NULL, false);
}

static void pump_tick(void *arg)
{
    (void)arg;
    if (uxQueueMessagesWaiting(s_q)) kick_pump();
}

static esp_err_t wifi_rx_cb(void *buffer, uint16_t len, void *eb)
{
    if (!s_bridging || !tud_ready()) { esp_wifi_internal_free_rx_buffer(eb); return ESP_OK; }
    sniff_dhcp_ack(buffer, len);
    pkt_t p = { .buf = buffer, .eb = eb, .len = len };
    if (xQueueSend(s_q, &p, 0) != pdTRUE) {             /* USB 全速口比 WiFi 慢，擠不下就丟，TCP 自己會降速 */
        esp_wifi_internal_free_rx_buffer(eb);
        s_st.drop_q++;
        return ESP_OK;
    }
    kick_pump();
    return ESP_OK;
}

/* ---------------- USB → WiFi ---------------- */

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (s_bridging) {
        esp_err_t e = ESP_FAIL;
        for (int i = 0; i < 20; i++) {                   /* WiFi 發送緩衝滿時稍等，最多 20 ms */
            e = esp_wifi_internal_tx(WIFI_IF_STA, (void *)src, size);
            if (e != ESP_ERR_NO_MEM) break;
            vTaskDelay(1);
        }
        if (e == ESP_OK) { s_st.to_wifi++; s_st.to_wifi_b += size; }
        else s_st.drop_tx++;
    }
    tud_network_recv_renew();
    return true;
}

void tud_network_init_cb(void)
{
    pkt_t p;
    while (s_q && xQueueReceive(s_q, &p, 0) == pdTRUE) esp_wifi_internal_free_rx_buffer(p.eb);
}

/* 目標機看到的「網線插沒插」= WiFi 連沒連上 */
bool tud_network_default_link_state_cb(void) { return s_bridging; }

/* ---------------- WiFi 連接管理 ---------------- */

static void set_bridging(bool on)
{
    if (on) {
        /* S3 自己的協議棧不再需要地址（收包都被轉走了），停掉它的 DHCP 客戶端免得和目標機搶 */
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) esp_netif_dhcpc_stop(sta);
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx_cb);
    }
    s_bridging = on;
    if (tud_mounted()) tud_network_link_state(0, on);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (!s_want) return;
    if (id == WIFI_EVENT_STA_DISCONNECTED && s_bridging) {
        set_bridging(false);
        kvm_cmd_event("net link=down");
    } else if (id == WIFI_EVENT_STA_CONNECTED && s_state == NET_UP) {
        /* board_wifi 自動重連上了。默認處理函數剛把收包回調改回 S3 自己的協議棧，這裡再搶回來 */
        set_bridging(true);
        kvm_cmd_event("net link=up");
    }
}

static void net_task(void *arg)
{
    (void)arg;
    s_state = NET_CONNECTING;
    board_wifi_set_hostname_auto("ramboot");
    kvm_ble_set_slow(true);                             /* WiFi 和 BLE 共用一個射頻，BLE 讓一讓 */
    esp_err_t e = board_wifi_connect(20000);
    if (!s_handlers) {                                  /* 要在 board_wifi 建好默認處理函數之後註冊，才排在它們後面 */
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_wifi_event, NULL);
        s_handlers = true;
    }
    if (!s_want) {                                      /* 連的過程中被叫停 */
            board_wifi_stop();
        kvm_ble_set_slow(false);
        s_state = NET_OFF;
    } else if (e != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 連不上: %s", esp_err_to_name(e));
        s_state = NET_FAILED;
        kvm_cmd_event("net err wifi:%s", esp_err_to_name(e));
    } else {
        board_wifi_stats_t ws;
        board_wifi_stats(&ws);
        s_state = NET_UP;
        set_bridging(true);
        kvm_cmd_event("net link=up wifi_rssi=%d", ws.rssi);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/*
 * WiFi 配置：home = Wifi.txt 裡的普通網絡；eap = 同一個文件裡的企業網絡段（如 eduroam）。
 * 憑據編譯時寫進固件（wifi_secrets.h），這裡只選用哪一套，存 NVS。board_wifi 的配置只在第一次開 WiFi 時套用一次，
 * 所以切換後要 reboot 才生效。
 */
static uint8_t s_wifi_prof;     /* 0 = home, 1 = eap */

static void load_wifi_profile(void)
{
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "wifiprof", &s_wifi_prof);
        nvs_close(h);
    }
#ifdef WIFI_EAP_SSID
    if (s_wifi_prof == 1) board_wifi_set_enterprise(WIFI_EAP_SSID, WIFI_EAP_ANON_ID, WIFI_EAP_IDENTITY, WIFI_EAP_PASSWORD);
#else
    s_wifi_prof = 0;
#endif
}

/* wifi            查詢
 * wifi home|eap   選配置並存下，回執裡 apply=reboot 表示要重啟才生效 */
static void cmd_wifi(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd;
#ifdef WIFI_EAP_SSID
    const char *avail = "home,eap";
#else
    const char *avail = WIFI_SSID[0] ? "home" : "none";
#endif
    if (!*args) { kvm_cmd_reply(seq, "ok wifi=%s avail=%s", s_wifi_prof ? "eap" : "home", avail); return; }
    int want = !strcmp(args, "eap") ? 1 : !strcmp(args, "home") ? 0 : -1;
    if (want < 0 || (want == 1 && strcmp(avail, "home,eap"))) { kvm_cmd_reply(seq, "err bad_arg:%s avail=%s", args, avail); return; }
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READWRITE, &h) != ESP_OK) { kvm_cmd_reply(seq, "err nvs"); return; }
    nvs_set_u8(h, "wifiprof", (uint8_t)want);
    nvs_commit(h);
    nvs_close(h);
    kvm_cmd_reply(seq, "ok wifi=%s saved apply=%s", want ? "eap" : "home", want == s_wifi_prof ? "none" : "reboot");
}

/* net   網卡轉發的狀態和計數 */
static void cmd_net(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd; (void)args;
    char info[260];
    kvm_net_info(info, sizeof(info));
    kvm_cmd_reply(seq, "ok %s ble_itvl=%dms", info, kvm_ble_itvl_ms());
}

void kvm_net_init(void)
{
    load_wifi_profile();
    kvm_cmd_register("wifi", cmd_wifi);
    kvm_cmd_register("net", cmd_net);
    esp_read_mac(tud_network_mac_address, ESP_MAC_WIFI_STA);
    s_q = xQueueCreate(QLEN, sizeof(pkt_t));
    const esp_timer_create_args_t a = { .callback = pump_tick, .name = "netpump" };
    esp_timer_create(&a, &s_pump_timer);
}

esp_err_t kvm_net_start(void)
{
    if (!WIFI_SSID[0] && !s_wifi_prof) return ESP_ERR_NOT_SUPPORTED;    /* 固件裡沒編進 WiFi 憑據 */
    if (s_want) return ESP_OK;
    if (s_task) return ESP_ERR_INVALID_STATE;           /* 上一次的收尾還沒跑完 */
    s_want = true;
    memset(&s_st, 0, sizeof(s_st));
    memset(s_ip, 0, sizeof(s_ip));
    esp_timer_start_periodic(s_pump_timer, 1000);
    if (xTaskCreate(net_task, "net", 4096, NULL, 5, &s_task) != pdPASS) { s_want = false; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

void kvm_net_stop(void)
{
    if (!s_want) return;
    s_want = false;
    esp_timer_stop(s_pump_timer);
    bool was_up = s_state == NET_UP || s_state == NET_FAILED;
    if (s_bridging) set_bridging(false);
    if (was_up) {                                       /* 還在連的話由 net_task 自己收尾 */
            board_wifi_stop();
        kvm_ble_set_slow(false);
        s_state = NET_OFF;
    }
    pkt_t p;
    while (xQueueReceive(s_q, &p, 0) == pdTRUE) esp_wifi_internal_free_rx_buffer(p.eb);
}

bool kvm_net_active(void) { return s_want || s_task != NULL; }

void kvm_net_info(char *buf, size_t len)
{
    static const char *const names[] = { "off", "connecting", "up", "failed" };
    int rssi = 0;
    if (s_state == NET_UP) {
        board_wifi_stats_t ws;
        board_wifi_stats(&ws);
        rssi = ws.rssi;
    }
    snprintf(buf, len, "net=%s link=%d wifi_rssi=%d ip=%u.%u.%u.%u to_usb_kb=%" PRIu32 " to_wifi_kb=%" PRIu32
             " to_usb_n=%" PRIu32 " to_wifi_n=%" PRIu32 " drop_q=%" PRIu32 " drop_tx=%" PRIu32,
             names[s_state], s_bridging, rssi, s_ip[0], s_ip[1], s_ip[2], s_ip[3],
             (uint32_t)(s_st.to_usb_b / 1024), (uint32_t)(s_st.to_wifi_b / 1024),
             s_st.to_usb, s_st.to_wifi, s_st.drop_q, s_st.drop_tx);
}
