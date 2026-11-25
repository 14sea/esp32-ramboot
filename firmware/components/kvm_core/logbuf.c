/*
 * 最近 8KB 日誌的環形副本。S3-01 的控制台在 UART0（CH343 口），平時那個口接充電頭，
 * 沒人看得到——日誌只能經 BLE 的 log 命令讀回來。
 */
#include "logbuf.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOGBUF_SIZE 8192

static char s_buf[LOGBUF_SIZE];
static size_t s_pos;
static bool s_wrapped;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_orig;

static int hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    char line[200];
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0 && xSemaphoreTake(s_lock, 0) == pdTRUE) {   /* 拿不到鎖就丟這一行，絕不阻塞調用方 */
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        for (int i = 0; i < n; i++) {
            char c = line[i];
            if (c == '\033') {                 /* 去掉顏色轉義序列 ESC[...m，經 BLE 讀回來時是噪音 */
                while (i < n && line[i] != 'm') i++;
                continue;
            }
            s_buf[s_pos++] = c;
            if (s_pos == LOGBUF_SIZE) { s_pos = 0; s_wrapped = true; }
        }
        xSemaphoreGive(s_lock);
    }
    return s_orig ? s_orig(fmt, ap) : 0;
}

void logbuf_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_orig = esp_log_set_vprintf(hook);
}

size_t logbuf_tail(char *out, size_t len)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    size_t have = s_wrapped ? LOGBUF_SIZE : s_pos;
    if (len > have) len = have;
    size_t start = (s_pos + LOGBUF_SIZE - len) % LOGBUF_SIZE;
    for (size_t i = 0; i < len; i++) out[i] = s_buf[(start + i) % LOGBUF_SIZE];
    xSemaphoreGive(s_lock);
    return len;
}
