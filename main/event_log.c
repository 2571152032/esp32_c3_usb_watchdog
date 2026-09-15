/**
 * @file event_log.c
 * @brief 事件日志实现 (RAM 环形缓冲 + NVS 持久化关键事件)
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "event_log.h"
#include "nvs_storage.h"

static const char *TAG = "EVT_LOG";

// NVS 存储句柄 (复用 watchdog 命名空间, 单句柄即可)
static nvs_handle_t s_nvs = 0;

static struct {
    bool initialized;
    log_entry_t ram[LOG_MAX_RAM_ENTRIES];
    uint32_t ram_head;       // 下一个写入位置
    uint32_t ram_count;      // 当前有效条数
    uint32_t seq_counter;
} s_log = {0};

// 互斥锁: event_log_write 会被多个任务 (system/watchdog/httpd/usb_rx) 并发调用,
// 无锁时环形缓冲索引与 NVS 环形头指针的读改写会互相踩踏, 导致日志丢失/错乱。
static SemaphoreHandle_t s_log_mutex = NULL;

#define LOG_LOCK()   do { if (s_log_mutex) xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)); } while (0)
#define LOG_UNLOCK() do { if (s_log_mutex) xSemaphoreGive(s_log_mutex); } while (0)

// NVS 环形: log_count / log_head / log_000 .. log_031
static uint32_t s_nvs_count = 0;
static uint32_t s_nvs_head  = 0;   // 下一个写入槽位

static const char *s_level_str[] = {"INFO", "WARN", "ERROR", "FATAL"};

const char *event_log_level_str(log_level_t level)
{
    if (level > LOG_LEVEL_FATAL) return "INFO";
    return s_level_str[level];
}

static void nvs_log_ensure(void)
{
    if (s_nvs) return;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs) == ESP_OK) {
        nvs_get_u32(s_nvs, "log_count", &s_nvs_count);
        nvs_get_u32(s_nvs, "log_head",  &s_nvs_head);
        if (s_nvs_count > LOG_MAX_NVS_ENTRIES) s_nvs_count = 0;
    }
}

static void nvs_log_persist(const log_entry_t *e)
{
    nvs_log_ensure();
    if (!s_nvs) return;

    char key[16];
    snprintf(key, sizeof(key), "log_%03lu", (unsigned long)s_nvs_head);

    // blob: level(1) + timestamp(4) + seq(4) + message(\0 terminated)
    uint8_t  buf[LOG_MAX_MESSAGE_LEN + 16];
    uint32_t off = 0;
    buf[off++] = (uint8_t)e->level;
    memcpy(buf + off, &e->timestamp_ms, 4); off += 4;
    memcpy(buf + off, &e->seq, 4);          off += 4;
    strncpy((char *)buf + off, e->message, LOG_MAX_MESSAGE_LEN - 1);
    buf[off + LOG_MAX_MESSAGE_LEN - 1] = '\0';   // strncpy 在源串恰好占满 n 字节时不补 NUL, 否则下方 strlen 越界读
    off += strlen((char *)buf + off) + 1;

    nvs_set_blob(s_nvs, key, buf, off);

    s_nvs_head = (s_nvs_head + 1) % LOG_MAX_NVS_ENTRIES;
    if (s_nvs_count < LOG_MAX_NVS_ENTRIES) s_nvs_count++;
    nvs_set_u32(s_nvs, "log_count", s_nvs_count);
    nvs_set_u32(s_nvs, "log_head",  s_nvs_head);
    nvs_commit(s_nvs);
}

esp_err_t event_log_init(void)
{
    if (s_log.initialized) return ESP_OK;
    memset(&s_log, 0, sizeof(s_log));
    s_log_mutex = xSemaphoreCreateMutex();
    s_log.initialized = true;

    nvs_log_ensure();

    ESP_LOGI(TAG, "Event log initialized (RAM=%d, NVS=%d)", LOG_MAX_RAM_ENTRIES, LOG_MAX_NVS_ENTRIES);
    LOG_I("System event log initialized");
    return ESP_OK;
}

void event_log_write(log_level_t level, const char *fmt, ...)
{
    if (!s_log.initialized) return;
    if (level > LOG_LEVEL_FATAL) level = LOG_LEVEL_INFO;

    LOG_LOCK();

    log_entry_t *e = &s_log.ram[s_log.ram_head];
    e->seq          = ++s_log.seq_counter;
    e->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e->level        = level;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, LOG_MAX_MESSAGE_LEN, fmt, ap);
    va_end(ap);

    s_log.ram_head = (s_log.ram_head + 1) % LOG_MAX_RAM_ENTRIES;
    if (s_log.ram_count < LOG_MAX_RAM_ENTRIES) s_log.ram_count++;

    // WARN 及以上持久化
    if (level >= LOG_LEVEL_WARN) {
        nvs_log_persist(e);
    }

    // 同步输出到 ESP_LOG
    ESP_LOGI(TAG, "[%s] %s", s_level_str[level], e->message);

    LOG_UNLOCK();
}

uint32_t event_log_get_ram_count(void) { return s_log.ram_count; }
uint32_t event_log_get_nvs_count(void) { return s_nvs_count; }

static uint32_t read_ring(const log_entry_t *ring, uint32_t head, uint32_t count,
                          log_entry_t *out, uint32_t max_count)
{
    if (!out || max_count == 0) return 0;
    uint32_t start = (count < LOG_MAX_RAM_ENTRIES) ? 0 : head;
    uint32_t copy  = (count < max_count) ? count : max_count;
    for (uint32_t i = 0; i < copy; i++) {
        memcpy(&out[i], &ring[(start + i) % LOG_MAX_RAM_ENTRIES], sizeof(log_entry_t));
    }
    return copy;
}

uint32_t event_log_read(log_entry_t *entries, uint32_t max_count)
{
    LOG_LOCK();
    uint32_t n = read_ring(s_log.ram, s_log.ram_head, s_log.ram_count, entries, max_count);
    LOG_UNLOCK();
    return n;
}

uint32_t event_log_read_nvs(log_entry_t *entries, uint32_t max_count)
{
    nvs_log_ensure();
    if (!s_nvs || !entries || max_count == 0) return 0;

    uint32_t start = (s_nvs_count < LOG_MAX_NVS_ENTRIES) ? 0 : s_nvs_head;
    uint32_t copy  = (s_nvs_count < max_count) ? s_nvs_count : max_count;

    for (uint32_t i = 0; i < copy; i++) {
        uint32_t   slot = (start + i) % LOG_MAX_NVS_ENTRIES;
        char       key[16];
        snprintf(key, sizeof(key), "log_%03lu", (unsigned long)slot);

        uint8_t  buf[LOG_MAX_MESSAGE_LEN + 16];
        size_t   len = sizeof(buf);
        if (nvs_get_blob(s_nvs, key, buf, &len) == ESP_OK && len > 9) {
            uint32_t off = 0;
            entries[i].level        = (log_level_t)buf[off++];
            memcpy(&entries[i].timestamp_ms, buf + off, 4); off += 4;
            memcpy(&entries[i].seq, buf + off, 4);          off += 4;
            strncpy(entries[i].message, (char *)buf + off, LOG_MAX_MESSAGE_LEN - 1);
            entries[i].message[LOG_MAX_MESSAGE_LEN - 1] = '\0';
        } else {
            memset(&entries[i], 0, sizeof(log_entry_t));
        }
    }
    return copy;
}

void event_log_clear_ram(void)
{
    LOG_LOCK();
    s_log.ram_count = 0;
    s_log.ram_head  = 0;
    s_log.seq_counter = 0;
    LOG_UNLOCK();
}

esp_err_t event_log_clear_nvs(void)
{
    nvs_log_ensure();
    if (!s_nvs) return ESP_FAIL;
    for (uint32_t i = 0; i < LOG_MAX_NVS_ENTRIES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "log_%03lu", (unsigned long)i);
        nvs_erase_key(s_nvs, key);
    }
    s_nvs_count = 0;
    s_nvs_head  = 0;
    nvs_set_u32(s_nvs, "log_count", 0);
    nvs_set_u32(s_nvs, "log_head",  0);
    nvs_commit(s_nvs);
    return ESP_OK;
}

const char *event_log_format_time(uint32_t timestamp_ms, char *buf, size_t buf_len)
{
    if (!buf || buf_len < 16) return "";
    uint32_t total_s = timestamp_ms / 1000;
    uint32_t h = (total_s / 3600) % 24;
    uint32_t m = (total_s / 60)  % 60;
    uint32_t s =  total_s % 60;
    snprintf(buf, buf_len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    return buf;
}
