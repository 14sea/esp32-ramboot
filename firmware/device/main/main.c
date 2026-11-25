/*
 * 設備端（接目標電腦的那塊板子）：BLE 命令通道 + USB 鍵盤/滑鼠 + 只讀啟動盤。
 * 功能都在 components/kvm_core。主機端工具 tools/kvm.py，經橋（firmware/bridge）連過來。
 * 要加命令（攝像頭、USB 網卡……）的工程：在 kvm_cmd_init() 和 kvm_ble_start() 之間調 kvm_cmd_register()。
 */
#include "esp_log.h"
#include "kvm_ble.h"
#include "kvm_cmd.h"
#include "kvm_hid.h"
#include "logbuf.h"
#include "nvs_flash.h"

static const char *TAG = "kvm";

void app_main(void)
{
    logbuf_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    kvm_cmd_init();             /* 先起命令層：USB/BLE 的回調會往它的隊列裡投事件 */
    ESP_ERROR_CHECK(kvm_hid_start());
    kvm_ble_start();
    ESP_LOGI(TAG, "KVM_READY ver=%s", KVM_FW_VERSION);
}
