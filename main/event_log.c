#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs.h"

#include "event_log.h"
#include "nvs_storage.h"

static const char *TAG = "EVT_LOG";

static nvs_handle_t s_nvs = 0;

static struct {
    bool initialized;
    log_entry_t ram[LOG_MAX_RAM_ENTRIES];
    uint32_t ram_head;
    uint32_t ram_count;
    uint32_t seq_counter;
} s_log = {0};

static SemaphoreHandle_t s_log_mutex = NULL;

#define LOG_LOCK()   do { if (s_log_mutex) xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)); } while (0)
#define LOG_UNLOCK() do { if (s_log_mutex) xSemaphoreGive(s_log_mutex); } while (0)

static uint32_t s_nvs_count = 0;
static uint32_t s_nvs_head  = 0;

static const char *s_level_str[] = {"INFO", "WARN", "ERROR", "FATAL"};

const char *event_log_level_str(log_level_t level)
{
    if (level > LOG_LEVEL_FATAL) return "INFO";
    return s_level_str[level];
}

// ==================== SNTP 网络时间同步 (北京时间) ====================

// Unix 秒有效性阈值 (2021-01-01). 低于它说明 SNTP 尚未同步, time() 仍是 1970 年
#define WALLCLOCK_MIN_VALID_S   1609459200UL

static bool s_sntp_started = false;

// SNTP 首次同步的时间基准: 用于反推同步前那些只有运行时间的日志条目
static uint32_t s_sync_wallclock_s  = 0;
static uint32_t s_sync_timestamp_ms = 0;

bool event_log_time_synced(void)
{
    time_t now = time(NULL);
    return (now > (time_t)WALLCLOCK_MIN_VALID_S);
}

// SNTP 同步完成回调 (lwip SNTP 线程上下文): 记录一条日志让用户知道时间已校准
static void sntp_time_sync_cb(struct timeval *tv)
{
    if (s_sync_wallclock_s == 0 && tv && tv->tv_sec > WALLCLOCK_MIN_VALID_S) {
        s_sync_wallclock_s  = (uint32_t)tv->tv_sec;
        s_sync_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    LOG_I("网络时间已同步 (北京时间)");
}

void event_log_init_sntp(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    // 时区: 中国标准时间 UTC+8 (POSIX TZ 格式, "CST-8" = UTC+8)
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // 双服务器冗余: 一个公共池 + 一个国内节点, 提高同步成功率
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(sntp_time_sync_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP time sync started (TZ=CST-8)");
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

    uint8_t  buf[LOG_MAX_MESSAGE_LEN + 20];
    uint32_t off = 0;
    // 最高位作为 v2 格式标志 (含 wallclock_s); level 只有 0-3, 不会冲突
    buf[off++] = (uint8_t)e->level | 0x80;
    memcpy(buf + off, &e->timestamp_ms, 4); off += 4;
    memcpy(buf + off, &e->seq, 4);          off += 4;
    memcpy(buf + off, &e->wallclock_s, 4);  off += 4;
    strncpy((char *)buf + off, e->message, LOG_MAX_MESSAGE_LEN - 1);
    buf[off + LOG_MAX_MESSAGE_LEN - 1] = '\0';
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
    LOG_I("系统事件日志已初始化");
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
    // SNTP 已同步则记录绝对时间; 未同步保持 0, 显示时退回运行时间
    e->wallclock_s  = event_log_time_synced() ? (uint32_t)time(NULL) : 0;
    e->level        = level;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, LOG_MAX_MESSAGE_LEN, fmt, ap);
    va_end(ap);

    s_log.ram_head = (s_log.ram_head + 1) % LOG_MAX_RAM_ENTRIES;
    if (s_log.ram_count < LOG_MAX_RAM_ENTRIES) s_log.ram_count++;

    if (level >= LOG_LEVEL_WARN) {
        nvs_log_persist(e);
    }

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

        uint8_t  buf[LOG_MAX_MESSAGE_LEN + 20];
        size_t   len = sizeof(buf);
        if (nvs_get_blob(s_nvs, key, buf, &len) == ESP_OK && len > 9) {
            uint32_t off = 0;
            uint8_t  lvl = buf[off++];
            bool     is_v2 = (lvl & 0x80) != 0;   // v2 含 wallclock_s
            entries[i].level = (log_level_t)(lvl & 0x7F);
            memcpy(&entries[i].timestamp_ms, buf + off, 4); off += 4;
            memcpy(&entries[i].seq, buf + off, 4);          off += 4;
            if (is_v2) {
                memcpy(&entries[i].wallclock_s, buf + off, 4); off += 4;
            } else {
                entries[i].wallclock_s = 0;   // 旧格式: 无绝对时间
            }
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

const char *event_log_format_time(uint32_t timestamp_ms, uint32_t wallclock_s,
                                  char *buf, size_t buf_len)
{
    if (!buf || buf_len < 24) return "";

    // 优先使用条目自带的绝对时间
    if (wallclock_s <= WALLCLOCK_MIN_VALID_S &&
        s_sync_wallclock_s > WALLCLOCK_MIN_VALID_S && s_sync_timestamp_ms > 0) {
        // SNTP 同步后, 根据同步基准反推这条日志发生时的北京时间
        int64_t diff_ms = (int64_t)s_sync_timestamp_ms - (int64_t)timestamp_ms;
        time_t  est     = (time_t)((int64_t)s_sync_wallclock_s - diff_ms / 1000);
        if (est > WALLCLOCK_MIN_VALID_S) {
            wallclock_s = (uint32_t)est;
        }
    }

    if (wallclock_s > WALLCLOCK_MIN_VALID_S) {
        time_t     t = (time_t)wallclock_s;
        struct tm  tm_now;
        localtime_r(&t, &tm_now);
        strftime(buf, buf_len, "%Y-%m-%d · %H-%M-%S", &tm_now);
        return buf;
    }

    // 无任何时间基准时, 退回显示启动后运行时间
    uint32_t total_s = timestamp_ms / 1000;
    uint32_t days = total_s / 86400;
    uint32_t h = (total_s / 3600) % 24;
    uint32_t m = (total_s / 60)  % 60;
    uint32_t s =  total_s % 60;
    if (days > 0) {
        snprintf(buf, buf_len, "D%lu %02lu:%02lu:%02lu",
                 (unsigned long)days, (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(buf, buf_len, "%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }
    return buf;
}
