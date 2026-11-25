/*
 * BLE 外設：一個自定義 GATT 服務（RX 寫 / TX 通知），按行收發。
 *
 * 安全：LE Secure Connections + 固定 6 位口令（S3-01 是「只能顯示」、對端輸入），
 * 禁用 Just Works；RX 特徵要求加密 + MITM 認證才能寫；綁定存 NVS。
 * 配對失敗 5 次後拒絕新配對直到重啟（口令只有 6 位，防暴力嘗試）；已綁定設備重連不受影響。
 */
#include "kvm_ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "kvm_cmd.h"
#include "kvm_proto.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

static const char *TAG = "kvm_ble";

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(KVM_UUID128(KVM_UUID_SVC));
static const ble_uuid128_t s_rx_uuid  = BLE_UUID128_INIT(KVM_UUID128(KVM_UUID_RX));
static const ble_uuid128_t s_tx_uuid  = BLE_UUID128_INIT(KVM_UUID128(KVM_UUID_TX));

static uint16_t s_tx_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_secure, s_subscribed;
static volatile uint16_t s_mtu = 23;
static volatile uint16_t s_itvl;        /* 當前連接間隔，單位 1.25ms */
static uint8_t s_own_addr_type;
static int s_pair_fail;
static char s_name[16];
static SemaphoreHandle_t s_tx_lock;
static esp_timer_handle_t s_auth_timer;
#define RX_RESERVE 20           /* 發送時至少給接收留這麼多空閒 mbuf 塊 */

static char s_rx[KVM_LINE_MAX];
static size_t s_rx_len;
static bool s_rx_overflow;

static void advertise(void);
static volatile bool s_quiet;       /* 控制通道換到了別的傳輸上（見 kvm_link.h）：不廣播、不接受連接 */

/* ---------------- GATT ---------------- */

static void rx_bytes(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\r') continue;
        if (c == '\n') {
            s_rx[s_rx_len] = '\0';
            if (s_rx_overflow) kvm_cmd_event("err line_too_long");
            else if (s_rx_len) kvm_cmd_post_line(s_rx);
            s_rx_len = 0;
            s_rx_overflow = false;
        } else if (s_rx_len < sizeof(s_rx) - 1) {
            s_rx[s_rx_len++] = c;
        } else {
            s_rx_overflow = true;
        }
    }
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t tmp[520];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(tmp)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    rx_bytes(tmp, len);
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = chr_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---------------- 發送 ---------------- */

bool kvm_ble_ready(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_secure && s_subscribed;
}

bool kvm_ble_send(const char *data, size_t len)
{
    if (!kvm_ble_ready()) return false;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    bool ok = true;
    while (len && ok) {
        size_t chunk = s_mtu > 3 ? s_mtu - 3 : 20;
        if (chunk > len) chunk = len;
        int64_t t0 = esp_timer_get_time();
        for (;;) {
            if (!kvm_ble_ready()) { ok = false; break; }
            /* 給接收留緩衝：發大圖時通知會把 mbuf 池佔滿，對端寫進來的心跳就被回
             * ATT 0x11（資源不足）。池裡空閒塊少於 RX_RESERVE 就先等控制器把發出去的消化掉 */
            if (os_msys_num_free() < RX_RESERVE && esp_timer_get_time() - t0 < 5000000) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data, chunk);
            /* notify_custom 無論成敗都會釋放 om。緩衝滿（mbuf 池或控制器隊列）時等一會重建再試——
             * 這就是流控：對端收得慢，這裡就慢下來。等滿 5 秒還發不出去才算失敗 */
            int rc = om ? ble_gatts_notify_custom(s_conn, s_tx_handle, om) : BLE_HS_ENOMEM;
            if (rc == 0) break;
            if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) || esp_timer_get_time() - t0 > 5000000) {
                ESP_LOGW(TAG, "通知發送失敗 rc=%d（%s）", rc, om ? "notify" : "mbuf 池耗盡");
                ok = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        data += chunk;
        len -= chunk;
    }
    xSemaphoreGive(s_tx_lock);
    return ok;
}

/*
 * BLE 和 WiFi 共用 2.4GHz 射頻分時工作。連接間隔 7.5–15ms 時 BLE 佔走大量時隙，WiFi 基本不可用
 * （2026-11-24 家裡實測：ping 網關丟六成、平均 400 ms，下載連不上）；放寬到 50–100ms 後只是中等影響（約 −25%）。
 * 所以 WiFi 開著時必須是慢間隔。
 *
 * 這裡只記「想要哪種」，由 itvl_keeper 定時核對實際間隔、不一致就重新請求。原來是調用時請求一次：
 *   - 上一次更新還沒完成時再請求會被拒（rc=2 BLE_HS_EALREADY），之後就一直停在錯的間隔上
 *   - BLE 重連後間隔回到默認的快速，沒人再去放慢
 * 這兩種情況都會讓 WiFi 在「以為已經讓路」的情況下被擠垮，而且時有時無，很難看出來。
 */
static volatile bool s_want_slow;
static esp_timer_handle_t s_itvl_timer;

static void itvl_keeper(void *arg)
{
    (void)arg;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_secure) return;
    bool is_slow = s_itvl >= 40;        /* 單位 1.25ms：40 = 50ms */
    if (is_slow == s_want_slow) return;
    struct ble_gap_upd_params p = {
        .itvl_min = s_want_slow ? 40 : 6,
        .itvl_max = s_want_slow ? 80 : 12,
        .latency = 0,
        .supervision_timeout = s_want_slow ? 600 : 200,    /* 單位 10ms，必須大於 2×間隔 */
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(s_conn, &p);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "請求連接間隔 %s rc=%d", s_want_slow ? "50–100ms" : "7.5–15ms", rc);
}

/* 當前連接間隔（毫秒），沒連著是 0。測速結果裡帶上它，免得又把「間隔沒放慢」誤當成別的原因 */
int kvm_ble_itvl_ms(void)
{
    return s_conn == BLE_HS_CONN_HANDLE_NONE ? 0 : (int)(s_itvl * 1.25f + 0.5f);
}

void kvm_ble_set_slow(bool slow)
{
    s_want_slow = slow;
    itvl_keeper(NULL);
}

/*
 * 靜默：BLE 只要在收發（廣播或連接事件），同一塊芯片上 WiFi 的批量傳輸就時通時停（2026-11-24 實測，
 * 放寬連接間隔、WiFi 省電、共存優先級都救不回來）。控制通道換到 ESP-NOW 之後把 BLE 徹底停掉；控制器開著但空閒沒有影響。
 */
void kvm_ble_set_quiet(bool quiet)
{
    s_quiet = quiet;
    if (quiet) {
        ble_gap_adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    } else if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        advertise();
    }
}

void kvm_ble_info(char *buf, size_t len)
{
    int8_t rssi = 0;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_conn_rssi(s_conn, &rssi);
    int bonds = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bonds);
    snprintf(buf, len, "ble=%s mtu=%u itvl=%.1fms rssi=%d bonds=%d pair_fail=%d",
             kvm_ble_ready() ? "up" : s_quiet ? "quiet" : (s_conn != BLE_HS_CONN_HANDLE_NONE ? "connecting" : "adv"),
             s_mtu, s_itvl * 1.25f, rssi, bonds, s_pair_fail);
}

/* ---------------- GAP ---------------- */

/* 連上 30 秒還沒完成加密認證就踢掉，免得未認證連接佔着唯一的連接位 */
static void auth_timeout(void *arg)
{
    (void)arg;
    uint16_t c = s_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE && !s_secure) {
        ESP_LOGW(TAG, "30 秒內未完成認證，斷開");
        ble_gap_terminate(c, BLE_ERR_AUTH_FAIL);
    }
}

static void check_link_up(void)
{
    if (kvm_ble_ready()) {
        ESP_LOGI(TAG, "鏈路就緒（已加密認證、已訂閱）");
        kvm_cmd_link_up();
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn = ev->connect.conn_handle;
        s_secure = s_subscribed = false;
        s_mtu = 23;
        s_rx_len = 0;
        s_rx_overflow = false;
        if (ble_gap_conn_find(s_conn, &desc) == 0) {
            s_itvl = desc.conn_itvl;
            const uint8_t *a = desc.peer_id_addr.val;
            ESP_LOGI(TAG, "連上 %02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
        }
        /* 等對端（S3-02）發起加密 */
        esp_timer_stop(s_auth_timer);
        esp_timer_start_once(s_auth_timer, 30 * 1000 * 1000);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "斷開 reason=0x%x", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_secure = s_subscribed = false;
        /* 靜默期間的斷開是換傳輸時自己斷的，鏈路狀態由換上去的那種傳輸負責報，也不重新廣播 */
        if (!s_quiet) {
            kvm_cmd_link_down();
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0 &&
            desc.sec_state.encrypted && desc.sec_state.authenticated) {
            s_secure = true;
            ESP_LOGI(TAG, "加密認證完成（bonded=%d key_size=%d）", desc.sec_state.bonded, desc.sec_state.key_size);
            check_link_up();
        } else {
            s_pair_fail++;
            ESP_LOGW(TAG, "加密/認證失敗 status=%d，累計 %d 次", ev->enc_change.status, s_pair_fail);
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = ev->subscribe.cur_notify;
            check_link_up();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &desc) == 0) {
            s_itvl = desc.conn_itvl;
            ESP_LOGI(TAG, "連接間隔 = %.2f ms", desc.conn_itvl * 1.25f);
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        /* 只有「新配對」才走到這裡；已綁定設備重連直接用存好的密鑰加密 */
        if (s_pair_fail >= 5) {
            ESP_LOGW(TAG, "配對失敗次數過多，拒絕新配對（重啟後解除）");
            ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
            return 0;
        }
        struct ble_sm_io io = { .action = ev->passkey.params.action };
        if (io.action == BLE_SM_IOACT_DISP) {
            io.passkey = KVM_PASSKEY;
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        } else {
            ESP_LOGW(TAG, "不支持的配對方式 action=%d（只接受口令輸入）", io.action);
            ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* 對端丟了綁定（比如 S3-02 重燒）又來配對：刪掉舊綁定，按新配對走（仍需口令） */
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

static void advertise(void)
{
    if (s_quiet) return;
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &s_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ESP_LOGI(TAG, "BLE 廣播中：%s", s_name);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 重置 reason=%d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void kvm_ble_start(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t ta = { .callback = auth_timeout, .name = "ble_auth" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_auth_timer));
    const esp_timer_create_args_t ka = { .callback = itvl_keeper, .name = "ble_itvl" };
    ESP_ERROR_CHECK(esp_timer_create(&ka, &s_itvl_timer));
    esp_timer_start_periodic(s_itvl_timer, 400 * 1000);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof(s_name), "kvm-%02x%02x", mac[4], mac[5]);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs));
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
