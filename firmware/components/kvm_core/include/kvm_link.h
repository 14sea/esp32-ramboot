#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * 控制鏈路：命令層只調 kvm_link_send / kvm_link_ready，不關心底下是哪種傳輸。
 * 本組件只有 BLE。引用本組件的工程可以另接一種傳輸（例如 WiFi 開著時用 ESP-NOW），
 * 用 kvm_link_set_alt() 換上去；換上、換下時由它自己調 kvm_cmd_link_down() / kvm_cmd_link_up()，
 * 並用 kvm_ble_set_quiet() 讓 BLE 靜默或恢復。
 */
typedef struct {
    bool (*ready)(void);
    bool (*send)(const char *data, size_t len);
} kvm_link_ops_t;

bool kvm_link_ready(void);
bool kvm_link_send(const char *data, size_t len);

/* ops 要一直有效；NULL = 回到 BLE */
void kvm_link_set_alt(const kvm_link_ops_t *ops);
