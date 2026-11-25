# 設備端的全部功能：命令層、BLE 外設、USB 鍵盤/滑鼠、只讀啟動盤。
# WHOLE_ARCHIVE：TinyUSB 的回調（tud_*_cb）在 usbd.c 裡有弱定義，不整庫鏈接的話這裡的強定義可能不被選上。
idf_component_register(SRCS "kvm_cmd.c" "kvm_ble.c" "kvm_hid.c" "kvm_keys.c" "kvm_link.c" "kvm_msc.c" "kvm_net.c" "kvm_target.c" "board_wifi.c" "logbuf.c"
                       INCLUDE_DIRS "include"
                       REQUIRES kvm_common esp_timer
                       PRIV_REQUIRES bt nvs_flash usb esp_hw_support esp_partition esp_system espressif__tinyusb
                                     esp_wifi wpa_supplicant esp_netif esp_event
                       WHOLE_ARCHIVE)

# 把本目錄的 tusb_config.h 交給 TinyUSB 組件
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
