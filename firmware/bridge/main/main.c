/*
 * 橋：USB 串口 ↔ BLE。只轉發，不處理。
 *
 * - 原生 USB-Serial-JTAG 口接主機，按行收發
 * - BLE 中心端：掃描廣播了 KVM 服務 UUID 的設備（firmware/device），連上、口令配對/綁定、訂閱通知
 * - 主機發來的行原樣寫給設備的 RX 特徵；設備的通知原樣寫回 USB
 * - '@' 開頭的行只在本板和主機之間：
 *     主機 → 本板：@status  @ping  @unpair（清綁定）  @reboot  @drop（主動斷開 BLE，測試失聯自保用）
 *                   @pause N（斷開並 N 秒內不重連）
 *     本板 → 主機：@boot  @up peer=.. mtu=..  @down reason=..  @status ..  @pong  @err ..
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "kvm_proto.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
/* 必須在 NimBLE 頭文件之後：它依賴 modlog 和 ble_gatt 的類型 */
#include "esp_central.h"

void ble_store_config_init(void);

#define BRIDGE_VERSION "0.3"

static const char *TAG = "bridge";

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(KVM_UUID128(KVM_UUID_SVC));
static const ble_uuid128_t s_rx_uuid  = BLE_UUID128_INIT(KVM_UUID128(KVM_UUID_RX));
static const ble_uuid128_t s_tx_uuid  = BLE_UUID128_INIT(KVM_UUID128(KVM_UUID_TX));

/* 連接參數：間隔 7.5–15ms 讓按鍵延遲低；監督超時 2 秒，斷線能較快發現 */
static const struct ble_gap_conn_params s_conn_params = {
    .scan_itvl = 0x0010, .scan_window = 0x0010,
    .itvl_min = 6, .itvl_max = 12, .latency = 0,
    .supervision_timeout = 200, .min_ce_len = 0, .max_ce_len = 0,
};

static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_handle, s_tx_handle;
static volatile bool s_ready;
static volatile uint16_t s_mtu = 23;
static char s_peer[18] = "-";
static SemaphoreHandle_t s_wr_done, s_usb_lock;
static volatile int s_wr_status;

static void scan(void);
static volatile int64_t s_pause_until;      /* @pause：這個時間之前不掃描、不重連 */


/* ---------------- USB 輸出 ---------------- */

static void usb_out(const char *s, size_t n)
{
    xSemaphoreTake(s_usb_lock, portMAX_DELAY);
    /* 主機沒打開串口時寫不出去，最多等 50ms 就丟，不能卡住 NimBLE 任務 */
    usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(50));
    xSemaphoreGive(s_usb_lock);
}

static void usb_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void usb_printf(const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    usb_out(buf, n);
}

/* ---------------- 服務發現 / 訂閱 ---------------- */

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err->status != 0) {
        ESP_LOGE(TAG, "訂閱失敗 status=%d", err->status);
        usb_printf("@err subscribe status=%d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    s_ready = true;
    ESP_LOGI(TAG, "就緒：%s mtu=%u", s_peer, s_mtu);
    usb_printf("@up peer=%s mtu=%u", s_peer, s_mtu);
    return 0;
}

static void on_disc(const struct peer *peer, int status, void *arg)
{
    (void)arg;
    if (status != 0) {
        usb_printf("@err discovery status=%d", status);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    const struct peer_chr *rx = peer_chr_find_uuid(peer, &s_svc_uuid.u, &s_rx_uuid.u);
    const struct peer_chr *tx = peer_chr_find_uuid(peer, &s_svc_uuid.u, &s_tx_uuid.u);
    const struct peer_dsc *cccd = peer_dsc_find_uuid(peer, &s_svc_uuid.u, &s_tx_uuid.u,
                                                     BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16));
    if (!rx || !tx || !cccd) {
        usb_printf("@err peer_lacks_kvm_service");
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    s_rx_handle = rx->chr.val_handle;
    s_tx_handle = tx->chr.val_handle;
    static const uint8_t on[2] = { 1, 0 };
    ble_gattc_write_flat(peer->conn_handle, cccd->dsc.handle, on, sizeof(on), on_subscribed, NULL);
}

/* ---------------- GAP ---------------- */

static bool is_kvm_adv(const struct ble_gap_disc_desc *d)
{
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) return false;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return false;
    for (int i = 0; i < f.num_uuids128; i++) {
        if (ble_uuid_cmp(&f.uuids128[i].u, &s_svc_uuid.u) == 0) return true;
    }
    return false;
}

static int gap_event(struct ble_gap_event *ev, void *arg);

static void connect_to(const struct ble_gap_disc_desc *d)
{
    if (ble_gap_disc_cancel() != 0) return;
    const uint8_t *a = d->addr.val;
    snprintf(s_peer, sizeof(s_peer), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
    ESP_LOGI(TAG, "發現設備 %s rssi=%d，連接", s_peer, d->rssi);
    int rc = ble_gap_connect(s_own_addr_type, &d->addr, 10000, &s_conn_params, gap_event, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "connect rc=%d", rc); scan(); }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (is_kvm_adv(&ev->disc)) connect_to(&ev->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) scan();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            ESP_LOGW(TAG, "連接失敗 status=%d", ev->connect.status);
            scan();
            return 0;
        }
        s_conn = ev->connect.conn_handle;
        s_mtu = 23;
        peer_add(s_conn);
        ble_gattc_exchange_mtu(s_conn, NULL, NULL);
        /* 先加密認證再做服務發現：RX 特徵要求認證，沒加密前寫會被拒 */
        ble_gap_security_initiate(s_conn);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0 &&
            desc.sec_state.authenticated) {
            ESP_LOGI(TAG, "加密認證完成（bonded=%d）", desc.sec_state.bonded);
            peer_disc_all(ev->enc_change.conn_handle, on_disc, NULL);
        } else {
            ESP_LOGW(TAG, "加密失敗 status=%d", ev->enc_change.status);
            usb_printf("@err encryption status=%d", ev->enc_change.status);
            /* 對端丟了綁定（S3-01 重燒/清過 NVS）：我們存的密鑰失效，刪掉，下次重新配對 */
            if (ev->enc_change.status == BLE_HS_ERR_HCI_BASE + BLE_ERR_PINKEY_MISSING &&
                ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
                usb_printf("@err peer_lost_bond deleted_local_bond");
            }
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = { .action = ev->passkey.params.action };
        if (io.action == BLE_SM_IOACT_INPUT) {
            io.passkey = KVM_PASSKEY;
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
            ESP_LOGI(TAG, "輸入配對口令");
        } else {
            ESP_LOGW(TAG, "意外的配對方式 action=%d", io.action);
            ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.attr_handle == s_tx_handle) {
            uint8_t buf[520];
            uint16_t n = 0;
            if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &n) == 0 && n) usb_out((char *)buf, n);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "斷開 reason=0x%x", ev->disconnect.reason);
        s_ready = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        peer_delete(ev->disconnect.conn.conn_handle);
        s_wr_status = BLE_HS_ENOTCONN;
        xSemaphoreGive(s_wr_done);      /* 喚醒可能在等寫回執的 USB 任務 */
        usb_printf("@down reason=0x%x", ev->disconnect.reason);
        scan();
        return 0;

    default:
        return 0;
    }
}

static void scan(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (esp_timer_get_time() < s_pause_until) return;
    struct ble_gap_disc_params p = { 0 };
    p.filter_duplicates = 1;
    p.passive = 1;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "scan rc=%d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ESP_LOGI(TAG, "掃描設備 ...");
    scan();
}

static void on_reset(int reason) { ESP_LOGE(TAG, "NimBLE 重置 reason=%d", reason); }

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------- USB → BLE ---------------- */

static int on_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    s_wr_status = err->status;
    xSemaphoreGive(s_wr_done);
    return 0;
}

/*
 * 按 MTU 分包，每包等寫回執（可靠、順序保證；命令都很短，延遲可以接受）。
 * 傳大圖時收到的通知會暫時佔滿 mbuf 池，寫請求拿不到緩衝（ENOMEM），等一會重試，最多 2 秒。
 * 失敗時 *why 填原因，給 @err 用。
 */
static bool ble_write(const char *s, size_t n, char *why, size_t why_len)
{
    int64_t t0 = esp_timer_get_time();     /* 每包重置，見末尾 */
    while (n) {
        if (!s_ready) { snprintf(why, why_len, "not_ready"); return false; }
        size_t chunk = s_mtu > 3 ? s_mtu - 3 : 20;
        if (chunk > n) chunk = n;
        int rc;
        for (;;) {
            xSemaphoreTake(s_wr_done, 0);
            rc = ble_gattc_write_flat(s_conn, s_rx_handle, s, chunk, on_written, NULL);
            if (rc == 0) break;
            if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) || esp_timer_get_time() - t0 > 2000000) {
                snprintf(why, why_len, "rc=%d", rc);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (xSemaphoreTake(s_wr_done, pdMS_TO_TICKS(3000)) != pdTRUE) { snprintf(why, why_len, "timeout"); return false; }
        /* S3-01 正忙着發大圖、接收緩衝不夠時回 ATT 0x11（資源不足）：等一會重寫這一包 */
        if (s_wr_status == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_INSUFFICIENT_RES &&
            esp_timer_get_time() - t0 < 2000000) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_wr_status != 0) { snprintf(why, why_len, "status=%d", s_wr_status); return false; }
        s += chunk;
        n -= chunk;
        t0 = esp_timer_get_time();
    }
    return true;
}

static void local_cmd(const char *line)
{
    if (strcmp(line, "@ping") == 0) {
        usb_printf("@pong");
    } else if (strcmp(line, "@status") == 0) {
        int8_t rssi = 0;
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_conn_rssi(s_conn, &rssi);
        int bonds = 0;
        ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &bonds);
        uint8_t m[6];
        esp_read_mac(m, ESP_MAC_WIFI_STA);
        usb_printf("@status bridge=%s ready=%d via=ble peer=%s mtu=%u rssi=%d bonds=%d uptime=%lld "
                   "mac=%02x:%02x:%02x:%02x:%02x:%02x",
                   BRIDGE_VERSION, s_ready, s_conn != BLE_HS_CONN_HANDLE_NONE ? s_peer : "-",
                   s_mtu, rssi, bonds, esp_timer_get_time() / 1000000, m[0], m[1], m[2], m[3], m[4], m[5]);
    } else if (strcmp(line, "@unpair") == 0) {
        ble_store_clear();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        usb_printf("@ok unpaired");
    } else if (strncmp(line, "@pause ", 7) == 0) {
        int sec = atoi(line + 7);
        if (sec < 1 || sec > 600) { usb_printf("@err usage: @pause 1..600"); return; }
        s_pause_until = esp_timer_get_time() + (int64_t)sec * 1000000;
        ble_gap_disc_cancel();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        static esp_timer_handle_t t;
        if (!t) {
            const esp_timer_create_args_t a = { .callback = (esp_timer_cb_t)scan, .name = "unpause" };
            esp_timer_create(&a, &t);
        }
        esp_timer_stop(t);
        esp_timer_start_once(t, (uint64_t)sec * 1000000 + 1000);
        usb_printf("@ok paused %ds", sec);
    } else if (strcmp(line, "@drop") == 0) {
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        usb_printf("@ok dropping");
    } else if (strcmp(line, "@reboot") == 0) {
        usb_printf("@ok rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        usb_printf("@err unknown_local_cmd");
    }
}

static void usb_task(void *arg)
{
    (void)arg;
    static char line[KVM_LINE_MAX + 2];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t buf[256];
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (len < KVM_LINE_MAX) line[len++] = c;
                else overflow = true;
                continue;
            }
            if (overflow) usb_printf("@err line_too_long");
            else if (len && line[0] == '@') { line[len] = '\0'; local_cmd(line); }
            else if (len) {
                line[len] = '\n';
                if (!s_ready) usb_printf("@err not_connected");
                else {
                    char why[24];
                    if (!ble_write(line, len + 1, why, sizeof(why))) usb_printf("@err write_failed %s", why);
                }
            }
            len = 0;
            overflow = false;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_wr_done = xSemaphoreCreateBinary();
    s_usb_lock = xSemaphoreCreateMutex();
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 2048;
    ucfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ESP_ERROR_CHECK(peer_init(1, 8, 16, 16));
    ble_svc_gap_device_name_set("kvm-bridge");
    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    xTaskCreate(usb_task, "usb_rx", 4096, NULL, 5, NULL);
    usb_printf("@boot bridge=%s", BRIDGE_VERSION);
    ESP_LOGI(TAG, "BRIDGE_READY");
}
