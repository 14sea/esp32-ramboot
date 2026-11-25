#pragma once
#include <stddef.h>

void logbuf_init(void);
/* 取最近 len 字節日誌，返回實際字節數 */
size_t logbuf_tail(char *out, size_t len);
