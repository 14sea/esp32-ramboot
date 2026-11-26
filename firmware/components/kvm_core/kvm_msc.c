/*
 * 只讀啟動盤：把 flash 的 bootimg 分區原樣當成一塊 512 字節扇區的 U 盤給目標電腦。
 * 分區裡放的是整盤鏡像（GPT + FAT 的 EFI 分區，boot/stage1/mkimage.sh 生成），S3 不解析文件系統。
 * 數據全在本地 flash，讀盤不經網絡——UEFI 讀 U 盤的逾時只有幾秒。
 * 只在 USB 模式含 msc 時才出現在描述符裡（見 kvm_hid.c）。
 */
#include "kvm_msc.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "kvm_cmd.h"
#include "kvm_hid.h"
#include "kvm_target.h"
#include "device/usbd_pvt.h"
#include "tusb.h"

static const char *TAG = "kvm_msc";

#define SECTOR 512

static const esp_partition_t *s_part;
static volatile uint32_t s_blocks;      /* 0 = 分區裡沒有可識別的鏡像，主機看到「無介質」 */
static volatile bool s_ejected;

static struct {
    uint32_t reads, errors, max_ms;
    uint64_t bytes;
    int64_t first_us, last_us;          /* 第一次/最後一次讀的時間，算平均速度用 */
    uint32_t inquiry, tur, other;       /* 收到的 SCSI 命令數，看主機有沒有在跟啟動盤說話 */
    uint8_t last_other;
} s_st;
/*
 * 追蹤：主機對這塊盤做了什麼。主機那邊看不到目標機的屏幕，「BIOS 讀了哪些扇區」是判斷它在幹什麼的主要線索：
 * 只讀第 0 扇區 = 傳統 BIOS 在找引導碼；讀分區表和 FAT 但不讀大文件 = 認了盤但沒從它啟動；連續讀幾 MB = 在加載內核。
 * 連續的讀合併成一條。mtrace 命令讀出來（最近 TRACE_N 條）。
 */
#define TRACE_N 96
static struct { uint32_t ms, lba, n; char op; } s_tr[TRACE_N];
static volatile uint32_t s_tr_total;

static void trace(char op, uint32_t lba, uint32_t n)
{
    uint32_t total = s_tr_total;
    if (op == 'R' && total) {               /* 接著上一條讀：合併 */
        uint32_t last = (total - 1) % TRACE_N;
        if (s_tr[last].op == 'R' && s_tr[last].lba + s_tr[last].n == lba) { s_tr[last].n += n; return; }
    }
    uint32_t i = total % TRACE_N;
    s_tr[i].ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_tr[i].lba = lba;
    s_tr[i].n = n;
    s_tr[i].op = op;
    s_tr_total = total + 1;
}

/* mtrace         每行 "| <開機毫秒> <操作> …"：M 枚舉完成，I 查詢，R 讀（起始扇區 + 扇區數），W 寫（被拒），
 *                E 彈出，C 其他 SCSI 命令（操作碼）。TEST UNIT READY 太多，不記
 * mtrace clear   清空 */
static void cmd_mtrace(uint32_t seq, const char *cmd, char *args)
{
    (void)cmd;
    if (!strcmp(args, "clear")) { s_tr_total = 0; kvm_cmd_reply(seq, "ok cleared"); return; }
    uint32_t total = s_tr_total, n = total < TRACE_N ? total : TRACE_N;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (total - n + k) % TRACE_N;
        char line[64];
        int len = s_tr[i].op == 'R' || s_tr[i].op == 'W'
                ? snprintf(line, sizeof(line), "| %" PRIu32 " %c lba=%" PRIu32 " n=%" PRIu32 "\n", s_tr[i].ms, s_tr[i].op, s_tr[i].lba, s_tr[i].n)
                : s_tr[i].op == 'C'
                ? snprintf(line, sizeof(line), "| %" PRIu32 " C op=%02" PRIx32 "\n", s_tr[i].ms, s_tr[i].lba)
                : snprintf(line, sizeof(line), "| %" PRIu32 " %c\n", s_tr[i].ms, s_tr[i].op);
        kvm_cmd_send(line, (size_t)len);
    }
    kvm_cmd_reply(seq, "ok n=%" PRIu32 " total=%" PRIu32 " now_ms=%lld", n, total, esp_timer_get_time() / 1000);
}

/* 本次枚舉以來讀了多少。主機只是認盤、掃分區表時讀幾百 KB；讀過 1 MB 基本就是固件在加載 Stage-1 的內核 */
static volatile uint32_t s_mount_bytes;
#define BOOT_READ_HINT (1024 * 1024)

/* 盤的大小取鏡像自己的大小，不是分區大小：GPT 的備份表頭必須在盤的最後一個扇區 */
static uint32_t detect_blocks(void)
{
    uint8_t sec[SECTOR];
    if (!s_part) return 0;
    uint32_t max = s_part->size / SECTOR;
    if (esp_partition_read(s_part, SECTOR, sec, SECTOR) == ESP_OK && memcmp(sec, "EFI PART", 8) == 0) {
        uint64_t alt;
        memcpy(&alt, sec + 32, 8);              /* AlternateLBA = 最後一個扇區 */
        if (alt > 0 && alt < max) return (uint32_t)alt + 1;
        ESP_LOGW(TAG, "GPT 備份表頭在 %" PRIu64 "，超出分區（%" PRIu32 " 扇區）", alt, max);
        return 0;
    }
    if (esp_partition_read(s_part, 0, sec, SECTOR) == ESP_OK && sec[510] == 0x55 && sec[511] == 0xAA) return max;
    return 0;
}

void kvm_msc_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bootimg");
    kvm_target_init();
    kvm_cmd_register("mtrace", cmd_mtrace);
    if (!s_part) { ESP_LOGW(TAG, "沒有 bootimg 分區"); return; }
    kvm_msc_rescan();
}

uint32_t kvm_msc_rescan(void)
{
    s_blocks = detect_blocks();
    s_ejected = false;
    memset(&s_st, 0, sizeof(s_st));
    ESP_LOGI(TAG, "bootimg 分區 %" PRIu32 " KB，鏡像 %" PRIu32 " 扇區", s_part ? s_part->size / 1024 : 0, s_blocks);
    return s_blocks;
}

void kvm_msc_bus_reset(void) { s_ejected = false; s_mount_bytes = 0; trace('M', 0, 0); }

void kvm_msc_info(char *buf, size_t len)
{
    uint32_t span_ms = (uint32_t)((s_st.last_us - s_st.first_us) / 1000);
    /* in_busy 長時間為 1 = 有一筆數據主機一直不來取（主機換驅動時丟下了半截命令） */
    snprintf(buf, len, "msc_blocks=%" PRIu32 " msc_eject=%d msc_rd_kb=%" PRIu32 " msc_rd_n=%" PRIu32
             " msc_rd_span_ms=%" PRIu32 " msc_rd_maxms=%" PRIu32 " msc_err=%" PRIu32
             " msc_inq=%" PRIu32 " msc_tur=%" PRIu32 " msc_other=%" PRIu32 "/%02x msc_in_busy=%d msc_out_busy=%d",
             s_blocks, s_ejected, (uint32_t)(s_st.bytes / 1024), s_st.reads, span_ms, s_st.max_ms, s_st.errors,
             s_st.inquiry, s_st.tur, s_st.other, s_st.last_other,
             usbd_edpt_busy(0, 0x83), usbd_edpt_busy(0, 0x03));
}

/* ---------------- TinyUSB MSC 回調 ---------------- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    s_st.inquiry++;
    trace('I', 0, 0);
    memcpy(vendor_id,   "DIY     ", 8);
    memcpy(product_id,  "S3 Boot Disk    ", 16);
    memcpy(product_rev, "0.5 ", 4);
}

static bool media_present(uint8_t lun)
{
    if (s_blocks && !s_ejected) return true;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);       /* MEDIUM NOT PRESENT */
    return false;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) { s_st.tur++; return media_present(lun); }

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_blocks;
    *block_size  = SECTOR;
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return false; }

/*
 * 主機「彈出」= 目標機裡的小系統說「我已經全在內存裡了，不再需要這塊盤」。
 * S3 只上報事件（"* msc eject"），接下來做什麼由 agent 決定。
 * 之後目標機重啟（總線復位、重新枚舉）時介質自動回來，否則 BIOS 找不到啟動盤。
 */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition;
    if (!load_eject) return true;
    if (start) { s_ejected = false; return true; }
    if (!s_ejected) { kvm_cmd_event("msc eject"); trace('E', 0, 0); }
    s_ejected = true;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (!media_present(lun)) return -1;
    /* TinyUSB 不拿 LBA 和容量比，自己把關；EP 緩衝是 512 的整數倍，每次都從扇區邊界開始 */
    uint32_t count = bufsize / SECTOR;
    if (offset != 0 || bufsize % SECTOR || lba >= s_blocks || count > s_blocks - lba) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);     /* LBA OUT OF RANGE */
        return -1;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_partition_read(s_part, (size_t)lba * SECTOR, buffer, bufsize);
    int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        s_st.errors++;
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);        /* UNRECOVERED READ ERROR */
        return -1;
    }
    uint32_t ms = (uint32_t)((t1 - t0) / 1000);
    if (ms > s_st.max_ms) s_st.max_ms = ms;
    if (!s_st.reads) s_st.first_us = t0;
    s_st.last_us = t1;
    s_st.reads++;
    s_st.bytes += bufsize;
    trace('R', lba, count);
    uint32_t before = s_mount_bytes;
    s_mount_bytes = before + bufsize;
    if (before < BOOT_READ_HINT && before + bufsize >= BOOT_READ_HINT) kvm_cmd_event("msc read_1mb");
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)offset; (void)buffer;
    trace('W', lba, bufsize / SECTOR);
    tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);            /* WRITE PROTECTED */
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    s_st.other++;
    s_st.last_other = scsi_cmd[0];
    if (scsi_cmd[0] != 0xFB) trace('C', scsi_cmd[0], 0);
    /* 廠商命令：操作碼 FB + 四個字母 */
    if (scsi_cmd[0] == 0xFB) {
        /* BOOT（無數據）：純 U 盤模式下沒有鍵盤燈暗號可用，留一條從目標機重燒的路 */
        if (memcmp(&scsi_cmd[1], "BOOT", 4) == 0) {
            kvm_hid_reboot_to_download();
            return 0;
        }
        /* TMSG（數據出）：目標機報進度。TinyUSB 收完數據才調到這裡，buffer 裡就是那段文本 */
        if (memcmp(&scsi_cmd[1], "TMSG", 4) == 0) {
            kvm_target_msg(buffer, bufsize);
            return 0;
        }
        /* GCFG（數據入）：目標機讀配置。多帶一個結尾的 0，空配置時也有 1 字節可回（回 0 字節會被當成失敗） */
        if (memcmp(&scsi_cmd[1], "GCFG", 4) == 0) {
            if (bufsize < 2) return -1;
            size_t n = kvm_target_cfg(buffer, bufsize - 1);
            ((char *)buffer)[n] = '\0';
            return (int32_t)(n + 1);
        }
        /* NEXT（數據出）："<模式> [秒]"：目標機要求「下次重啟時換成這個模式」，同主機的 mode next */
        if (memcmp(&scsi_cmd[1], "NEXT", 4) == 0) {
            char arg[40], name[16] = "";
            size_t n = bufsize < sizeof(arg) - 1 ? bufsize : sizeof(arg) - 1;
            memcpy(arg, buffer, n);
            arg[n] = '\0';
            int back = 0;
            if (sscanf(arg, "%15s %d", name, &back) >= 1 && kvm_hid_set_next(name, back) == ESP_OK) {
                kvm_cmd_event("usb next armed mode=%s back_s=%d by=target", name, back);
                return 0;
            }
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x26, 0x00);  /* INVALID FIELD IN PARAMETER LIST */
            return -1;
        }
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);         /* INVALID COMMAND OPERATION CODE */
    return -1;
}
