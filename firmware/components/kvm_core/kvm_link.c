#include "kvm_link.h"

#include "kvm_ble.h"

static const kvm_link_ops_t *volatile s_alt;

bool kvm_link_ready(void)
{
    const kvm_link_ops_t *a = s_alt;
    return a ? a->ready() : kvm_ble_ready();
}

bool kvm_link_send(const char *data, size_t len)
{
    const kvm_link_ops_t *a = s_alt;
    return a ? a->send(data, len) : kvm_ble_send(data, len);
}

void kvm_link_set_alt(const kvm_link_ops_t *ops) { s_alt = ops; }
